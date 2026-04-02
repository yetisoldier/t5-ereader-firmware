#include "cover_renderer.h"

#include <Arduino.h>
#include <algorithm>
#include <stdint.h>
#include <vector>
#include "epub.h"
#include "display.h"
#include "image_tone.h"
#include <JPEGDEC.h>
#include <PNGdec.h>

struct ThumbContext {
    int srcW = 0;
    int srcH = 0;
    int dstX = 0;
    int dstY = 0;
    int dstW = 0;
    int dstH = 0;
    uint8_t* pixels = nullptr; // 8-bit grayscale
};

struct ThumbCacheEntry {
    String key;
    int w = 0;
    int h = 0;
    uint8_t* pixels = nullptr;
    uint32_t lastUse = 0;
};

static ThumbContext* g_thumbCtx = nullptr;
static JPEGDEC g_jpeg;
static PNG g_png;
static std::vector<ThumbCacheEntry> g_thumbCache;
static const size_t MAX_THUMB_CACHE = 6;

bool cover_can_render_poster(const BookInfo& book) {
    if (!book.hasCover || book.coverPath.length() == 0) return false;
    String lower = book.coverPath;
    lower.toLowerCase();
    return lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png");
}

static void blit_gray_to_thumb(ThumbContext* ctx, int sx, int sy, uint8_t gray) {
    if (!ctx || !ctx->pixels || ctx->srcW <= 0 || ctx->srcH <= 0) return;
    int dx = (sx * ctx->dstW) / ctx->srcW;
    int dy = (sy * ctx->dstH) / ctx->srcH;
    if (dx < 0 || dy < 0 || dx >= ctx->dstW || dy >= ctx->dstH) return;
    ctx->pixels[dy * ctx->dstW + dx] = gray;
}

static int jpegDrawCallback(JPEGDRAW* pDraw) {
    if (!g_thumbCtx || !pDraw) return 0;

    for (int yy = 0; yy < pDraw->iHeight; yy++) {
        for (int xx = 0; xx < pDraw->iWidth; xx++) {
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            blit_gray_to_thumb(g_thumbCtx, pDraw->x + xx, pDraw->y + yy, image_rgb565_to_gray8(px));
        }
    }

    return 1;
}

static int pngDrawCallback(PNGDRAW* pDraw) {
    if (!g_thumbCtx || !pDraw) return 0;

    static uint16_t lineBuffer[1024];
    if (pDraw->iWidth > 1024) return 0;

    g_png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
        blit_gray_to_thumb(g_thumbCtx, xx, pDraw->y, image_rgb565_to_gray8(lineBuffer[xx]));
    }

    return 1;
}

static bool ends_with_ci(const String& value, const char* suffix) {
    String lower = value;
    lower.toLowerCase();
    String suf = String(suffix);
    suf.toLowerCase();
    return lower.endsWith(suf);
}

static void draw_thumb_pixels(int dstX, int dstY, int w, int h, const uint8_t* pixels) {
    if (!pixels) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t gray8 = pixels[y * w + x];
            uint8_t gray4 = gray8 >> 4;  // 0=black, 15=white — EPD convention
            display_draw_pixel(dstX + x, dstY + y, gray4);
        }
    }
}

static String make_cache_key(const BookInfo& book, int w, int h) {
    return book.filepath + "|" + book.coverPath + "|" + String(w) + "x" + String(h);
}

static ThumbCacheEntry* find_cache_entry(const String& key) {
    for (auto& entry : g_thumbCache) {
        if (entry.key == key) {
            entry.lastUse = millis();
            return &entry;
        }
    }
    return nullptr;
}

static void evict_oldest_cache_entry() {
    if (g_thumbCache.empty()) return;
    size_t oldestIdx = 0;
    uint32_t oldestUse = g_thumbCache[0].lastUse;
    for (size_t i = 1; i < g_thumbCache.size(); i++) {
        if (g_thumbCache[i].lastUse < oldestUse) {
            oldestUse = g_thumbCache[i].lastUse;
            oldestIdx = i;
        }
    }
    free(g_thumbCache[oldestIdx].pixels);
    g_thumbCache.erase(g_thumbCache.begin() + oldestIdx);
}

void cover_cache_clear() {
    for (auto& entry : g_thumbCache) {
        free(entry.pixels);
    }
    g_thumbCache.clear();
}

static void store_cache_entry(const String& key, const ThumbContext& ctx) {
    if (!ctx.pixels || ctx.dstW <= 0 || ctx.dstH <= 0) return;
    if (g_thumbCache.size() >= MAX_THUMB_CACHE) {
        evict_oldest_cache_entry();
    }

    ThumbCacheEntry entry;
    entry.key = key;
    entry.w = ctx.dstW;
    entry.h = ctx.dstH;
    entry.lastUse = millis();
    size_t bytes = (size_t)ctx.dstW * ctx.dstH;
    entry.pixels = (uint8_t*)ps_malloc(bytes);
    if (!entry.pixels) return;
    memcpy(entry.pixels, ctx.pixels, bytes);
    g_thumbCache.push_back(entry);
}

bool cover_render_poster(BookInfo& book, int x, int y, int w, int h) {
    if (!cover_can_render_poster(book)) return false;

    int innerPad = 12;
    int maxW = std::max(1, w - innerPad * 2);
    int maxH = std::max(1, h - innerPad * 2);
    String cacheKey = make_cache_key(book, maxW, maxH);
    if (ThumbCacheEntry* cached = find_cache_entry(cacheKey)) {
        display_draw_filled_rect(x, y, w, h, 15);
        display_draw_rect(x, y, w, h, 8);
        int dstX = x + (w - cached->w) / 2;
        int dstY = y + (h - cached->h) / 2;
        draw_thumb_pixels(dstX, dstY, cached->w, cached->h, cached->pixels);
        display_draw_rect(x + 6, y + 6, w - 12, h - 12, 12);
        return true;
    }

    EpubParser parser;
    if (!parser.open(book.filepath.c_str())) return false;

    size_t size = 0;
    uint8_t* data = parser.readAsset(book.coverPath, &size);
    parser.close();
    if (!data || size == 0) {
        if (data) free(data);
        return false;
    }

    bool isJpeg = ends_with_ci(book.coverPath, ".jpg") || ends_with_ci(book.coverPath, ".jpeg");
    bool isPng = ends_with_ci(book.coverPath, ".png");
    if (!isJpeg && !isPng) {
        free(data);
        return false;
    }

    ThumbContext ctx;
    ctx.pixels = (uint8_t*)ps_malloc((size_t)maxW * maxH);
    if (!ctx.pixels) {
        free(data);
        return false;
    }
    memset(ctx.pixels, 255, (size_t)maxW * maxH);

    bool ok = false;
    if (isJpeg) {
        if (g_jpeg.openRAM(data, (int)size, jpegDrawCallback)) {
            ctx.srcW = g_jpeg.getWidth();
            ctx.srcH = g_jpeg.getHeight();
            if (ctx.srcW > 0 && ctx.srcH > 0) {
                int drawW = maxW;
                int drawH = (ctx.srcH * drawW) / ctx.srcW;
                if (drawH > maxH) {
                    drawH = maxH;
                    drawW = (ctx.srcW * drawH) / ctx.srcH;
                }
                ctx.dstW = std::max(1, drawW);
                ctx.dstH = std::max(1, drawH);
                ctx.dstX = x + (w - ctx.dstW) / 2;
                ctx.dstY = y + (h - ctx.dstH) / 2;
                g_thumbCtx = &ctx;
                ok = g_jpeg.decode(0, 0, 0) == 1;
                g_thumbCtx = nullptr;
            }
            g_jpeg.close();
        }
    } else if (isPng) {
        if (g_png.openRAM(data, (int)size, pngDrawCallback) == PNG_SUCCESS) {
            ctx.srcW = g_png.getWidth();
            ctx.srcH = g_png.getHeight();
            if (ctx.srcW > 0 && ctx.srcH > 0) {
                int drawW = maxW;
                int drawH = (ctx.srcH * drawW) / ctx.srcW;
                if (drawH > maxH) {
                    drawH = maxH;
                    drawW = (ctx.srcW * drawH) / ctx.srcH;
                }
                ctx.dstW = std::max(1, drawW);
                ctx.dstH = std::max(1, drawH);
                ctx.dstX = x + (w - ctx.dstW) / 2;
                ctx.dstY = y + (h - ctx.dstH) / 2;
                g_thumbCtx = &ctx;
                ok = g_png.decode(nullptr, 0) == PNG_SUCCESS;
                g_thumbCtx = nullptr;
            }
            g_png.close();
        }
    }

    if (ok) {
        display_draw_filled_rect(x, y, w, h, 15);
        display_draw_rect(x, y, w, h, 8);
        draw_thumb_pixels(ctx.dstX, ctx.dstY, ctx.dstW, ctx.dstH, ctx.pixels);
        display_draw_rect(x + 6, y + 6, w - 12, h - 12, 12);
        store_cache_entry(cacheKey, ctx);
    } else {
        book.posterCoverFailed = true;
    }

    free(ctx.pixels);
    free(data);
    return ok;
}
