#include "cover_renderer.h"

#include <Arduino.h>
#include <algorithm>
#include <stdint.h>
#include <string.h>
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

static void invalidate_cache_entry(const String& key) {
    for (auto it = g_thumbCache.begin(); it != g_thumbCache.end(); ++it) {
        if (it->key == key) {
            free(it->pixels);
            g_thumbCache.erase(it);
            return;
        }
    }
}

static uint64_t fnv1a64_init() {
    return 1469598103934665603ULL;
}

static uint64_t fnv1a64_update(uint64_t hash, const uint8_t* data, size_t len) {
    if (!data) return hash;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)((value >> 16) & 0xff),
        (uint8_t)((value >> 24) & 0xff),
    };
    return fnv1a64_update(hash, bytes, sizeof(bytes));
}

static uint64_t hash_string_ci(uint64_t hash, const String& value) {
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        hash ^= (uint8_t)c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t fingerprint_decoded_pixels(const uint8_t* pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return 0;
    uint64_t hash = fnv1a64_init();
    hash = hash_u32(hash, (uint32_t)w);
    hash = hash_u32(hash, (uint32_t)h);
    return fnv1a64_update(hash, pixels, (size_t)w * h);
}

static uint64_t fingerprint_asset_bytes(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;
    uint64_t hash = fnv1a64_init();
    uint64_t size64 = (uint64_t)size;
    hash = hash_u32(hash, (uint32_t)(size64 & 0xffffffffULL));
    hash = hash_u32(hash, (uint32_t)((size64 >> 32) & 0xffffffffULL));
    return fnv1a64_update(hash, data, size);
}

static bool path_has_placeholder_token(const String& path) {
    if (path.length() == 0) return false;

    static const char* TOKENS[] = {
        "placeholder",
        "defaultcover",
        "default_cover",
        "default-cover",
        "nocover",
        "no_cover",
        "no-cover",
        "missingcover",
        "missing_cover",
        "missing-cover",
        "covermissing",
        "cover_missing",
        "cover-missing",
        "brokenimage",
        "broken_image",
        "broken-image",
        "notfound",
        "not_found",
        "not-found",
        "fallback",
        "blankcover",
        "blank_cover",
        "blank-cover",
        "emptycover",
        "empty_cover",
        "empty-cover",
    };

    String lower = path;
    lower.toLowerCase();
    for (const char* token : TOKENS) {
        if (lower.indexOf(token) >= 0) return true;
    }
    return false;
}

struct PlaceholderMatchResult {
    bool matched = false;
    const char* reason = nullptr;
    uint64_t assetFingerprint = 0;
    uint64_t pixelFingerprint = 0;
    uint64_t pathFingerprint = 0;
};

static PlaceholderMatchResult classify_placeholder_or_degenerate(const String& coverPath,
                                                                const uint8_t* assetData,
                                                                size_t assetSize,
                                                                const uint8_t* pixels,
                                                                int w,
                                                                int h) {
    PlaceholderMatchResult result;
    if (!pixels || w <= 0 || h <= 0) return result;

    result.assetFingerprint = fingerprint_asset_bytes(assetData, assetSize);
    result.pixelFingerprint = fingerprint_decoded_pixels(pixels, w, h);
    result.pathFingerprint = hash_string_ci(fnv1a64_init(), coverPath);

    if (assetSize > 0 && assetSize < 1024) {
        result.matched = true;
        result.reason = "tiny-asset";
        return result;
    }

    if (w < 40 || h < 40) {
        result.matched = true;
        result.reason = "tiny-decode";
        return result;
    }

    if (path_has_placeholder_token(coverPath)) {
        result.matched = true;
        result.reason = "placeholder-path";
        return result;
    }

    return result;
}

bool cover_render_poster(BookInfo& book, int x, int y, int w, int h) {
    if (!cover_can_render_poster(book)) return false;

    int innerPad = 12;
    int maxW = std::max(1, w - innerPad * 2);
    int maxH = std::max(1, h - innerPad * 2);
    String cacheKey = make_cache_key(book, maxW, maxH);
    if (ThumbCacheEntry* cached = find_cache_entry(cacheKey)) {
        PlaceholderMatchResult cachedMatch = classify_placeholder_or_degenerate(book.coverPath,
                                                                                nullptr,
                                                                                0,
                                                                                cached->pixels,
                                                                                cached->w,
                                                                                cached->h);
        if (cachedMatch.matched) {
            Serial.printf("Poster cache invalidated: %s cached poster matched %s (path=%016llx pixels=%016llx)\n",
                          book.filepath.c_str(),
                          cachedMatch.reason ? cachedMatch.reason : "placeholder",
                          (unsigned long long)cachedMatch.pathFingerprint,
                          (unsigned long long)cachedMatch.pixelFingerprint);
            invalidate_cache_entry(cacheKey);
        } else {
            display_draw_filled_rect(x, y, w, h, 15);
            display_draw_rect(x, y, w, h, 8);
            int dstX = x + (w - cached->w) / 2;
            int dstY = y + (h - cached->h) / 2;
            draw_thumb_pixels(dstX, dstY, cached->w, cached->h, cached->pixels);
            display_draw_rect(x + 6, y + 6, w - 12, h - 12, 12);
            return true;
        }
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
        PlaceholderMatchResult match = classify_placeholder_or_degenerate(book.coverPath,
                                                                          data,
                                                                          size,
                                                                          ctx.pixels,
                                                                          ctx.dstW,
                                                                          ctx.dstH);
        if (match.matched) {
            Serial.printf("Poster fallback: %s cover asset rejected (%s) path=%016llx asset=%016llx pixels=%016llx cover=%s\n",
                          book.filepath.c_str(),
                          match.reason ? match.reason : "placeholder",
                          (unsigned long long)match.pathFingerprint,
                          (unsigned long long)match.assetFingerprint,
                          (unsigned long long)match.pixelFingerprint,
                          book.coverPath.c_str());
            invalidate_cache_entry(cacheKey);
            ok = false;
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
