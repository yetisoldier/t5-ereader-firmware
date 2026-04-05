#include "cover_renderer.h"

#include <Arduino.h>
#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "epub.h"
#include "display.h"
#include "config.h"
#include "image_tone.h"
#include "settings.h"
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
static const size_t MAX_THUMB_CACHE = 24;  // Increased from 16 to hold 3+ pages of poster view for pre-caching

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

static void invalidate_cache_entry(const String& key) {
    for (auto it = g_thumbCache.begin(); it != g_thumbCache.end(); ++it) {
        if (it->key == key) {
            free(it->pixels);
            g_thumbCache.erase(it);
            return;
        }
    }
}

static bool cover_asset_quality_ok(size_t assetSize, int decodedW, int decodedH) {
    if (assetSize < 1500) return false;
    if (decodedW < 100 || decodedH < 100) return false;
    if (decodedW <= 0 || decodedH <= 0) return false;

    float aspect = (float)decodedH / (float)decodedW;
    if (aspect < 0.5f || aspect > 4.0f) return false;

    return true;
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

    auto failPoster = [&](const char* reason) -> bool {
        Serial.printf("Poster fallback: %s cover rejected (%s) cover=%s\n",
                      book.filepath.c_str(),
                      reason ? reason : "unknown",
                      book.coverPath.c_str());
        invalidate_cache_entry(cacheKey);
        book.posterCoverFailed = true;
        return false;
    };

    EpubParser parser;
    if (!parser.open(book.filepath.c_str())) return failPoster("epub-open-failed");

    size_t size = 0;
    uint8_t* data = parser.readAsset(book.coverPath, &size);
    parser.close();
    if (!data || size == 0) {
        if (data) free(data);
        return failPoster("asset-read-failed");
    }

    if (size < 1500) {
        free(data);
        return failPoster("asset-too-small");
    }

    bool isJpeg = ends_with_ci(book.coverPath, ".jpg") || ends_with_ci(book.coverPath, ".jpeg");
    bool isPng = ends_with_ci(book.coverPath, ".png");
    if (!isJpeg && !isPng) {
        free(data);
        return failPoster("unsupported-cover-format");
    }

    ThumbContext ctx;
    ctx.pixels = (uint8_t*)ps_malloc((size_t)maxW * maxH);
    if (!ctx.pixels) {
        free(data);
        return failPoster("thumb-alloc-failed");
    }
    memset(ctx.pixels, 255, (size_t)maxW * maxH);

    bool decoderOpened = false;
    bool ok = false;
    if (isJpeg) {
        decoderOpened = g_jpeg.openRAM(data, (int)size, jpegDrawCallback);
        if (decoderOpened) {
            ctx.srcW = g_jpeg.getWidth();
            ctx.srcH = g_jpeg.getHeight();
            if (cover_asset_quality_ok(size, ctx.srcW, ctx.srcH)) {
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
        decoderOpened = g_png.openRAM(data, (int)size, pngDrawCallback) == PNG_SUCCESS;
        if (decoderOpened) {
            ctx.srcW = g_png.getWidth();
            ctx.srcH = g_png.getHeight();
            if (cover_asset_quality_ok(size, ctx.srcW, ctx.srcH)) {
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

    if (!decoderOpened) {
        free(ctx.pixels);
        free(data);
        return failPoster("decode-open-failed");
    }

    if (!cover_asset_quality_ok(size, ctx.srcW, ctx.srcH)) {
        free(ctx.pixels);
        free(data);
        return failPoster("failed-quality-guards");
    }

    if (!ok || !ctx.pixels || ctx.dstW <= 0 || ctx.dstH <= 0) {
        free(ctx.pixels);
        free(data);
        return failPoster("decode-failed");
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

// Pre-cache covers for adjacent pages to warm the cache
void cover_precache_page(std::vector<BookInfo>& books, const std::vector<int>& filteredIndices, int scroll, int cardsPerPage) {
    if (!settings_get().posterShowCovers) return;  // Only needed for poster view
    
    const int cols = 2;
    const int gap = 18;
    int posterW = (PORTRAIT_W - MARGIN_X * 2 - gap) / cols;
    int posterH = 310;
    
    // Pre-cache next page (scroll + cardsPerPage to scroll + 2*cardsPerPage - 1)
    int nextStart = scroll + cardsPerPage;
    int nextEnd = std::min(nextStart + cardsPerPage, (int)filteredIndices.size());
    
    for (int vi = nextStart; vi < nextEnd; vi++) {
        int bi = filteredIndices[vi];
        BookInfo& book = books[bi];
        
        if (!cover_can_render_poster(book) || book.posterCoverFailed) continue;
        
        int maxW = posterW - 24;  // innerPad * 2
        int maxH = posterH - 24;
        String cacheKey = make_cache_key(book, maxW, maxH);
        
        // Skip if already cached
        if (find_cache_entry(cacheKey)) continue;
        
        // Render to cache only (no display)
        EpubParser parser;
        if (!parser.open(book.filepath.c_str())) continue;
        
        size_t size = 0;
        uint8_t* data = parser.readAsset(book.coverPath, &size);
        parser.close();
        
        if (!data || size < 1500) {
            if (data) free(data);
            continue;
        }
        
        bool isJpeg = ends_with_ci(book.coverPath, ".jpg") || ends_with_ci(book.coverPath, ".jpeg");
        bool isPng = ends_with_ci(book.coverPath, ".png");
        if (!isJpeg && !isPng) {
            free(data);
            continue;
        }
        
        ThumbContext ctx;
        ctx.pixels = (uint8_t*)ps_malloc((size_t)maxW * maxH);
        if (!ctx.pixels) {
            free(data);
            continue;
        }
        memset(ctx.pixels, 255, (size_t)maxW * maxH);
        
        bool decoderOpened = false;
        bool ok = false;
        
        if (isJpeg) {
            decoderOpened = g_jpeg.openRAM(data, (int)size, jpegDrawCallback);
            if (decoderOpened) {
                ctx.srcW = g_jpeg.getWidth();
                ctx.srcH = g_jpeg.getHeight();
                if (cover_asset_quality_ok(size, ctx.srcW, ctx.srcH)) {
                    int drawW = maxW;
                    int drawH = (ctx.srcH * drawW) / ctx.srcW;
                    if (drawH > maxH) {
                        drawH = maxH;
                        drawW = (ctx.srcW * drawH) / ctx.srcH;
                    }
                    ctx.dstW = std::max(1, drawW);
                    ctx.dstH = std::max(1, drawH);
                    g_thumbCtx = &ctx;
                    ok = g_jpeg.decode(0, 0, 0) == 1;
                    g_thumbCtx = nullptr;
                }
                g_jpeg.close();
            }
        } else if (isPng) {
            decoderOpened = g_png.openRAM(data, (int)size, pngDrawCallback) == PNG_SUCCESS;
            if (decoderOpened) {
                ctx.srcW = g_png.getWidth();
                ctx.srcH = g_png.getHeight();
                if (cover_asset_quality_ok(size, ctx.srcW, ctx.srcH)) {
                    int drawW = maxW;
                    int drawH = (ctx.srcH * drawW) / ctx.srcW;
                    if (drawH > maxH) {
                        drawH = maxH;
                        drawW = (ctx.srcW * drawH) / ctx.srcH;
                    }
                    ctx.dstW = std::max(1, drawW);
                    ctx.dstH = std::max(1, drawH);
                    g_thumbCtx = &ctx;
                    ok = g_png.decode(nullptr, 0) == PNG_SUCCESS;
                    g_thumbCtx = nullptr;
                }
                g_png.close();
            }
        }
        
        if (ok && ctx.pixels && ctx.dstW > 0 && ctx.dstH > 0) {
            store_cache_entry(cacheKey, ctx);
        }
        
        free(ctx.pixels);
        free(data);
    }
}
