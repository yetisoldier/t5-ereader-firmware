#include "inline_image.h"
#include "epub.h"
#include "display.h"
#include "image_tone.h"
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <algorithm>

// ─── Render context shared with decode callbacks ───────────────────
struct ImgDrawCtx {
    int srcW = 0, srcH = 0;
    int dstX = 0, dstY = 0;
    int dstW = 0, dstH = 0;
    uint16_t* pngLineBuf = nullptr;
};

static ImgDrawCtx* g_ictx = nullptr;
// Decoders are heap-allocated on demand to avoid ~63KB of static BSS
// that starved epdiy's I2S DMA buffers.
static PNG* g_ipng_active = nullptr;  // set during decode for callback access

// ─── Pixel helpers ─────────────────────────────────────────────────

static void plot(int sx, int sy, uint8_t g4) {
    if (!g_ictx || g_ictx->srcW <= 0 || g_ictx->srcH <= 0) return;
    int dx = g_ictx->dstX + (sx * g_ictx->dstW) / g_ictx->srcW;
    int dy = g_ictx->dstY + (sy * g_ictx->dstH) / g_ictx->srcH;
    if (dx < 0 || dy < 0 || dx >= display_width() || dy >= display_height()) return;
    display_draw_pixel(dx, dy, g4);
}

// ─── JPEGDEC callback ──────────────────────────────────────────────

static int ijpegDraw(JPEGDRAW* pDraw) {
    if (!g_ictx || !pDraw) return 0;
    for (int yy = 0; yy < pDraw->iHeight; yy++) {
        for (int xx = 0; xx < pDraw->iWidth; xx++) {
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            plot(pDraw->x + xx, pDraw->y + yy, image_rgb565_to_gray4(px));
        }
    }
    return 1;
}

// ─── PNGdec callback ───────────────────────────────────────────────

static int ipngDraw(PNGDRAW* pDraw) {
    if (!g_ictx || !pDraw || !g_ictx->pngLineBuf || !g_ipng_active) return 0;
    g_ipng_active->getLineAsRGB565(pDraw, g_ictx->pngLineBuf, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
        plot(xx, pDraw->y, image_rgb565_to_gray4(g_ictx->pngLineBuf[xx]));
    }
    return 1;
}

// ─── Marker helpers ────────────────────────────────────────────────

bool inline_image_is_marker(const String& line) {
    return line.length() > 5 && line[0] == IMG_MARKER_BYTE &&
           line.startsWith(IMG_MARKER_PREFIX);
}

bool inline_image_is_continuation(const String& line) {
    return line == IMG_CONT_MARKER;
}

bool inline_image_parse_raw(const String& line, String& outPath) {
    // Format: \x01IMG|relativePath\x01
    if (!inline_image_is_marker(line)) return false;
    int start = 5; // skip \x01IMG|
    int end = line.indexOf('\x01', start);
    if (end < 0) end = line.length();
    outPath = line.substring(start, end);
    return outPath.length() > 0;
}

bool inline_image_parse_enriched(const String& line, String& outPath,
                                 int& outW, int& outH, int& outLines) {
    // Format: \x01IMG|zipPath|w|h|lines\x01
    if (!inline_image_is_marker(line)) return false;
    int p1 = 5; // after \x01IMG|
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

String inline_image_build_marker(const String& zipPath, int w, int h, int lines) {
    return String(IMG_MARKER_BYTE) + "IMG|" + zipPath + "|" +
           String(w) + "|" + String(h) + "|" + String(lines) +
           String(IMG_MARKER_BYTE);
}

// ─── Format detection ──────────────────────────────────────────────

static bool isJpeg(const String& path) {
    String lower = path;
    lower.toLowerCase();
    return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

static bool isPng(const String& path) {
    String lower = path;
    lower.toLowerCase();
    return lower.endsWith(".png");
}

static bool isSupportedImage(const String& path) {
    return isJpeg(path) || isPng(path);
}

// ─── Aspect-ratio scaling ──────────────────────────────────────────

static void scaleToFit(int srcW, int srcH, int maxW, int maxH,
                       int& outW, int& outH) {
    if (srcW <= 0 || srcH <= 0) { outW = outH = 0; return; }
    float scaleW = (float)maxW / srcW;
    float scaleH = (float)maxH / srcH;
    float scale = std::min(scaleW, scaleH);
    if (scale > 1.0f) scale = 1.0f;  // don't upscale
    outW = std::max(1, (int)(srcW * scale));
    outH = std::max(1, (int)(srcH * scale));
}

// ─── Probe: read dimensions without full decode ────────────────────

bool inline_image_probe(EpubParser& parser, const String& zipPath,
                        int maxW, int maxH, InlineImageInfo& out) {
    if (!isSupportedImage(zipPath)) return false;

    size_t dataSize = 0;
    uint8_t* data = parser.readAsset(zipPath, &dataSize);
    if (!data || dataSize == 0) return false;

    int imgW = 0, imgH = 0;

    if (isJpeg(zipPath)) {
        JPEGDEC* jpeg = new JPEGDEC();
        if (jpeg && jpeg->openRAM(data, (int)dataSize, ijpegDraw)) {
            imgW = jpeg->getWidth();
            imgH = jpeg->getHeight();
            jpeg->close();
        }
        delete jpeg;
    } else {
        PNG* png = new PNG();
        if (png && png->openRAM(data, (int)dataSize, ipngDraw) == PNG_SUCCESS) {
            imgW = png->getWidth();
            imgH = png->getHeight();
            png->close();
        }
        delete png;
    }

    free(data);

    if (imgW <= 0 || imgH <= 0) return false;

    // Skip tiny decorative images (icons, bullets, spacers)
    if (imgW < 10 && imgH < 10) return false;

    out.zipPath = zipPath;
    scaleToFit(imgW, imgH, maxW, maxH, out.displayW, out.displayH);
    return true;
}

// ─── Render: full decode to portrait framebuffer ───────────────────

bool inline_image_render(EpubParser& parser, const String& zipPath,
                         int dstX, int dstY, int dstW, int dstH) {
    if (!isSupportedImage(zipPath) || dstW <= 0 || dstH <= 0) return false;

    // JPEG/PNG decoders need ~10-15KB of regular heap for working state
    if ((int)ESP.getFreeHeap() < 25000) {
        Serial.printf("Skip image render: low heap (%d)\n", (int)ESP.getFreeHeap());
        return false;
    }

    size_t dataSize = 0;
    uint8_t* data = parser.readAsset(zipPath, &dataSize);
    if (!data || dataSize == 0) return false;

    ImgDrawCtx ctx;
    bool ok = false;

    if (isJpeg(zipPath)) {
        JPEGDEC* jpeg = new JPEGDEC();
        if (jpeg && jpeg->openRAM(data, (int)dataSize, ijpegDraw)) {
            ctx.srcW = jpeg->getWidth();
            ctx.srcH = jpeg->getHeight();
            ctx.dstX = dstX; ctx.dstY = dstY;
            ctx.dstW = dstW; ctx.dstH = dstH;
            g_ictx = &ctx;
            ok = jpeg->decode(0, 0, 0) == 1;
            g_ictx = nullptr;
            jpeg->close();
        }
        delete jpeg;
    } else {
        PNG* png = new PNG();
        if (png && png->openRAM(data, (int)dataSize, ipngDraw) == PNG_SUCCESS) {
            ctx.srcW = png->getWidth();
            ctx.srcH = png->getHeight();
            ctx.dstX = dstX; ctx.dstY = dstY;
            ctx.dstW = dstW; ctx.dstH = dstH;
            ctx.pngLineBuf = (uint16_t*)ps_malloc(ctx.srcW * sizeof(uint16_t));
            if (ctx.pngLineBuf) {
                g_ipng_active = png;  // set for callback access
                g_ictx = &ctx;
                ok = png->decode(nullptr, 0) == PNG_SUCCESS;
                g_ictx = nullptr;
                g_ipng_active = nullptr;
                free(ctx.pngLineBuf);
            }
            png->close();
        }
        delete png;
    }

    free(data);
    return ok;
}
