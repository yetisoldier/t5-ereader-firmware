#include "inline_image.h"
#include "epub.h"
#include "display.h"
#include "image_tone.h"
#include "debug_trace.h"
#include "config.h"
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <SD.h>
#include <algorithm>
#include <cstdio>

struct ImgDrawCtx {
    int srcW = 0, srcH = 0;
    int dstX = 0, dstY = 0;
    int dstW = 0, dstH = 0;
    uint16_t* pngLineBuf = nullptr;
    uint32_t deadlineMs = 0;
    uint32_t lastYieldMs = 0;
    bool aborted = false;
};

static ImgDrawCtx* g_ictx = nullptr;
static PNG* g_ipng_active = nullptr;

static bool inline_image_maybe_abort_decode() {
    if (!g_ictx) return false;
    uint32_t now = millis();
    if (g_ictx->deadlineMs && (int32_t)(now - g_ictx->deadlineMs) >= 0) {
        g_ictx->aborted = true;
        return true;
    }
    if (now - g_ictx->lastYieldMs >= 16) {
        yield();
        g_ictx->lastYieldMs = now;
    }
    return false;
}

static inline void plot_scaled(int sx, int sy, uint8_t gray4) {
    if (!g_ictx || g_ictx->srcW <= 0 || g_ictx->srcH <= 0) return;
    int dx = g_ictx->dstX + (sx * g_ictx->dstW) / g_ictx->srcW;
    int dy = g_ictx->dstY + (sy * g_ictx->dstH) / g_ictx->srcH;
    if (dx < 0 || dy < 0 || dx >= display_width() || dy >= display_height()) return;
    display_draw_pixel(dx, dy, gray4);
}

static int ijpegDraw(JPEGDRAW* pDraw) {
    if (!g_ictx || !pDraw) return 0;
    for (int yy = 0; yy < pDraw->iHeight; yy++) {
        if (inline_image_maybe_abort_decode()) return 0;
        for (int xx = 0; xx < pDraw->iWidth; xx++) {
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            plot_scaled(pDraw->x + xx, pDraw->y + yy, image_rgb565_to_gray4(px));
        }
    }
    return 1;
}

static int ipngDraw(PNGDRAW* pDraw) {
    if (!g_ictx || !pDraw || !g_ictx->pngLineBuf || !g_ipng_active) return 0;
    if (inline_image_maybe_abort_decode()) return 0;
    if (pDraw->iWidth <= 0 || pDraw->iWidth > 1024) return 0;

    g_ipng_active->getLineAsRGB565(pDraw, g_ictx->pngLineBuf, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
        plot_scaled(xx, pDraw->y, image_rgb565_to_gray4(g_ictx->pngLineBuf[xx]));
    }
    return 1;
}

static int noopJpegDraw(JPEGDRAW*) { return 1; }
static int noopPngDraw(PNGDRAW*) { return 1; }

static File* openSdFileHandle(const char* path, int32_t* outSize) {
    if (outSize) *outSize = 0;
    File* file = new File(SD.open(path, FILE_READ));
    if (!file || !(*file) || file->isDirectory()) {
        if (file) {
            if (*file) file->close();
            delete file;
        }
        return nullptr;
    }
    if (outSize) *outSize = (int32_t)file->size();
    return file;
}

static void* jpegFileOpen(const char* path, int32_t* outSize) {
    return openSdFileHandle(path, outSize);
}

static int32_t jpegFileRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
    if (!pFile || !pFile->fHandle) return 0;
    int32_t n = (int32_t)((File*)pFile->fHandle)->read(pBuf, len);
    if (n > 0) pFile->iPos += n;
    return n > 0 ? n : 0;
}

static int32_t jpegFileSeek(JPEGFILE* pFile, int32_t position) {
    if (!pFile || !pFile->fHandle) return -1;
    if (!((File*)pFile->fHandle)->seek(position)) return -1;
    pFile->iPos = position;
    return position;
}

static void jpegFileClose(void* handle) {
    if (!handle) return;
    File* file = (File*)handle;
    file->close();
    delete file;
}

static void* pngFileOpen(const char* path, int32_t* outSize) {
    return openSdFileHandle(path, outSize);
}

static int32_t pngFileRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
    if (!pFile || !pFile->fHandle) return 0;
    return (int32_t)((File*)pFile->fHandle)->read(pBuf, len);
}

static int32_t pngFileSeek(PNGFILE* pFile, int32_t position) {
    if (!pFile || !pFile->fHandle) return 0;
    return ((File*)pFile->fHandle)->seek(position) ? position : 0;
}

static void pngFileClose(void* handle) {
    if (!handle) return;
    File* file = (File*)handle;
    file->close();
    delete file;
}

bool inline_image_is_marker(const String& line) {
    return line.length() > 5 && line[0] == IMG_MARKER_BYTE &&
           line.startsWith(IMG_MARKER_PREFIX);
}

bool inline_image_is_continuation(const String& line) {
    return line == IMG_CONT_MARKER;
}

bool inline_image_parse_raw(const String& line, String& outPath) {
    if (!inline_image_is_marker(line)) return false;
    int start = 5;
    int end = line.indexOf('\x01', start);
    if (end < 0) end = line.length();
    outPath = line.substring(start, end);
    return outPath.length() > 0;
}

bool inline_image_parse_enriched(const String& line, String& outPath,
                                 int& outW, int& outH, int& outLines) {
    if (!inline_image_is_marker(line)) return false;
    int p1 = 5;
    int p2 = line.indexOf('|', p1);
    if (p2 < 0) return false;
    int p3 = line.indexOf('|', p2 + 1);
    if (p3 < 0) return false;
    int p4 = line.indexOf('|', p3 + 1);
    if (p4 < 0) return false;
    int p5 = line.indexOf('\x01', p4 + 1);
    if (p5 < 0) p5 = line.length();

    outPath = line.substring(p1, p2);
    outW = line.substring(p2 + 1, p3).toInt();
    outH = line.substring(p3 + 1, p4).toInt();
    outLines = line.substring(p4 + 1, p5).toInt();
    return outPath.length() > 0 && outW > 0 && outH > 0 && outLines > 0;
}

String inline_image_build_marker(const String& assetPath, int w, int h, int lines) {
    return String(IMG_MARKER_BYTE) + "IMG|" + assetPath + "|" +
           String(w) + "|" + String(h) + "|" + String(lines) +
           String(IMG_MARKER_BYTE);
}

static bool ends_with_ci(const String& value, const char* suffix) {
    String lower = value;
    lower.toLowerCase();
    String suf = String(suffix);
    suf.toLowerCase();
    return lower.endsWith(suf);
}

static bool isJpeg(const String& path) {
    return ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
}

static bool isPng(const String& path) {
    return ends_with_ci(path, ".png");
}

static bool isSupportedImage(const String& path) {
    return isJpeg(path) || isPng(path);
}

static String image_extension(const String& path) {
    if (ends_with_ci(path, ".jpeg")) return ".jpeg";
    if (ends_with_ci(path, ".jpg")) return ".jpg";
    if (ends_with_ci(path, ".png")) return ".png";
    return ".img";
}

static void scaleToFit(int srcW, int srcH, int maxW, int maxH,
                       int& outW, int& outH) {
    if (srcW <= 0 || srcH <= 0) { outW = outH = 0; return; }
    float scaleW = (float)maxW / srcW;
    float scaleH = (float)maxH / srcH;
    float scale = std::min(scaleW, scaleH);
    if (scale > 1.0f) scale = 1.0f;
    outW = std::max(1, (int)(srcW * scale));
    outH = std::max(1, (int)(srcH * scale));
}

static bool inline_image_asset_ok(size_t assetSize, int decodedW, int decodedH, bool png) {
    if (assetSize == 0 || assetSize > 8 * 1024 * 1024) return false;
    if (decodedW <= 0 || decodedH <= 0) return false;
    if (decodedW > 4096 || decodedH > 4096) return false;
    uint64_t pixels = (uint64_t)decodedW * (uint64_t)decodedH;
    if (pixels > 2ULL * 1024ULL * 1024ULL) return false;
    if (png && decodedW > 1024) return false;
    return true;
}

static String vfs_path(const String& path) {
    if (path.startsWith("/sd")) return path;
    return String("/sd") + path;
}

static uint32_t fnv1a_hash(const String& a, const String& b) {
    uint32_t h = 2166136261u;
    auto mix = [&](const String& s) {
        for (int i = 0; i < (int)s.length(); i++) {
            h ^= (uint8_t)s[i];
            h *= 16777619u;
        }
    };
    mix(a);
    h ^= (uint8_t)'|';
    h *= 16777619u;
    mix(b);
    return h;
}

static String inline_cache_dir() {
    return String(LINE_CACHE_DIR) + "/inline";
}

static bool ensure_inline_cache_dir() {
    if (!SD.exists(LINE_CACHE_DIR) && !SD.mkdir(LINE_CACHE_DIR)) return false;
    String dir = inline_cache_dir();
    if (SD.exists(dir)) return true;
    return SD.mkdir(dir);
}

static String inline_cache_path_for(const String& bookPath, const String& zipPath) {
    uint32_t h = fnv1a_hash(bookPath, zipPath);
    return inline_cache_dir() + "/img_" + String(h, HEX) + image_extension(zipPath);
}

static bool file_has_content(const String& path, size_t* outSize = nullptr) {
    if (outSize) *outSize = 0;
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return false;
    }
    size_t sz = f.size();
    f.close();
    if (outSize) *outSize = sz;
    return sz > 0;
}

static bool extract_asset_to_cache(EpubParser& parser, const String& bookPath,
                                   const String& zipPath, String& outPath, size_t* outSize) {
    if (outSize) *outSize = 0;
    if (!isSupportedImage(zipPath)) return false;
    if (!ensure_inline_cache_dir()) return false;

    outPath = inline_cache_path_for(bookPath, zipPath);
    if (file_has_content(outPath, outSize)) return true;

    size_t dataSize = 0;
    uint8_t* data = parser.readAsset(zipPath, &dataSize);
    if (!data || dataSize == 0) {
        if (data) free(data);
        return false;
    }
    if (dataSize > 8 * 1024 * 1024) {
        free(data);
        return false;
    }

    String tmpPath = outPath + ".tmp";
    String tmpVfs = vfs_path(tmpPath);
    String finalVfs = vfs_path(outPath);

    FILE* f = fopen(tmpVfs.c_str(), "wb");
    if (!f) {
        free(data);
        return false;
    }

    bool ok = fwrite(data, 1, dataSize, f) == dataSize;
    fclose(f);
    free(data);

    if (!ok) {
        remove(tmpVfs.c_str());
        return false;
    }

    remove(finalVfs.c_str());
    if (rename(tmpVfs.c_str(), finalVfs.c_str()) != 0) {
        remove(tmpVfs.c_str());
        return false;
    }

    if (outSize) *outSize = dataSize;
    return true;
}

static bool probe_image_file(const String& assetPath, int& imgW, int& imgH, size_t* outSize) {
    imgW = 0;
    imgH = 0;
    size_t assetSize = 0;
    if (!file_has_content(assetPath, &assetSize)) return false;
    if (outSize) *outSize = assetSize;

    if (isJpeg(assetPath)) {
        JPEGDEC* jpeg = new JPEGDEC();
        if (!jpeg) return false;
        bool ok = jpeg->open(assetPath.c_str(), jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, noopJpegDraw);
        if (ok) {
            imgW = jpeg->getWidth();
            imgH = jpeg->getHeight();
            jpeg->close();
        }
        delete jpeg;
        return ok;
    }

    PNG* png = new PNG();
    if (!png) return false;
    bool ok = png->open(assetPath.c_str(), pngFileOpen, pngFileClose, pngFileRead, pngFileSeek, noopPngDraw) == PNG_SUCCESS;
    if (ok) {
        imgW = png->getWidth();
        imgH = png->getHeight();
        png->close();
    }
    delete png;
    return ok;
}

bool inline_image_probe(EpubParser& parser, const String& bookPath, const String& zipPath,
                        int maxW, int maxH, InlineImageInfo& out) {
    debug_trace_mark("inline_image_probe:start", zipPath);
    if (!isSupportedImage(zipPath)) return false;

    String assetPath;
    size_t assetSize = 0;
    if (!extract_asset_to_cache(parser, bookPath, zipPath, assetPath, &assetSize)) return false;

    int imgW = 0, imgH = 0;
    if (!probe_image_file(assetPath, imgW, imgH, &assetSize)) return false;

    debug_trace_mark("inline_image_probe:decoded", String(imgW) + "x" + String(imgH));
    if (!inline_image_asset_ok(assetSize, imgW, imgH, isPng(assetPath))) return false;
    if (imgW < 10 && imgH < 10) return false;

    out.assetPath = assetPath;
    scaleToFit(imgW, imgH, maxW, maxH, out.displayW, out.displayH);
    return out.displayW > 0 && out.displayH > 0;
}

bool inline_image_render(const String& assetPath,
                         int dstX, int dstY, int dstW, int dstH) {
    debug_trace_mark("inline_image_render:start", assetPath);
    if (!isSupportedImage(assetPath) || dstW <= 0 || dstH <= 0) return false;
    if ((int)ESP.getFreeHeap() < 20000) return false;

    size_t assetSize = 0;
    if (!file_has_content(assetPath, &assetSize)) return false;

    ImgDrawCtx ctx;
    ctx.dstX = dstX;
    ctx.dstY = dstY;
    ctx.dstW = dstW;
    ctx.dstH = dstH;
    ctx.deadlineMs = millis() + 1500;
    ctx.lastYieldMs = millis();
    bool ok = false;

    if (isJpeg(assetPath)) {
        debug_trace_mark("inline_image_render:jpeg", assetPath);
        JPEGDEC jpeg;
        if (jpeg.open(assetPath.c_str(), jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, ijpegDraw)) {
            ctx.srcW = jpeg.getWidth();
            ctx.srcH = jpeg.getHeight();
            if (inline_image_asset_ok(assetSize, ctx.srcW, ctx.srcH, false)) {
                g_ictx = &ctx;
                ok = jpeg.decode(0, 0, 0) == 1;
                g_ictx = nullptr;
                if (ctx.aborted) debug_trace_mark("inline_image_render:jpeg_timeout", assetPath);
            }
            jpeg.close();
        }
    } else if (isPng(assetPath)) {
        debug_trace_mark("inline_image_render:png", assetPath);
        PNG png;
        if (png.open(assetPath.c_str(), pngFileOpen, pngFileClose, pngFileRead, pngFileSeek, ipngDraw) == PNG_SUCCESS) {
            ctx.srcW = png.getWidth();
            ctx.srcH = png.getHeight();
            if (inline_image_asset_ok(assetSize, ctx.srcW, ctx.srcH, true)) {
                ctx.pngLineBuf = (uint16_t*)ps_malloc((size_t)ctx.srcW * sizeof(uint16_t));
                if (ctx.pngLineBuf) {
                    g_ictx = &ctx;
                    g_ipng_active = &png;
                    ok = png.decode(nullptr, 0) == PNG_SUCCESS;
                    g_ipng_active = nullptr;
                    g_ictx = nullptr;
                    if (ctx.aborted) debug_trace_mark("inline_image_render:png_timeout", assetPath);
                    free(ctx.pngLineBuf);
                    ctx.pngLineBuf = nullptr;
                } else {
                    debug_trace_mark("inline_image_render:png_linebuf_alloc_failed", String(ctx.srcW));
                }
            }
            png.close();
        }
    }

    debug_trace_mark("inline_image_render:done", ok ? "ok" : "fail");
    return ok;
}
