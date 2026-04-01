#include "sleep_image.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include "config.h"
#include "display.h"

struct SleepDrawContext {
    int srcW = 0;
    int srcH = 0;
    int dstX = 0;
    int dstY = 0;
    int dstW = 0;
    int dstH = 0;
    // Per-row RGB565 line buffer allocated from PSRAM based on actual image width.
    // Previously a static lineBuffer[1024] was used, which silently aborted any
    // PNG decode for images wider than 1024 px (most photos are 1920+ px wide).
    uint16_t* pngLineBuffer = nullptr;
};

static SleepDrawContext* g_ctx = nullptr;
static JPEGDEC g_jpeg;
static PNG g_png;
static Preferences g_prefs;

static inline uint8_t rgb565_to_gray4(uint16_t px) {
    uint8_t r = ((px >> 11) & 0x1F) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    uint8_t gray8 = (uint8_t)((r * 38 + g * 75 + b * 15) >> 7);
    return 15 - (gray8 >> 4);
}

static void plot_scaled(int sx, int sy, uint8_t gray4) {
    if (!g_ctx || g_ctx->srcW <= 0 || g_ctx->srcH <= 0) return;
    int dx = g_ctx->dstX + (sx * g_ctx->dstW) / g_ctx->srcW;
    int dy = g_ctx->dstY + (sy * g_ctx->dstH) / g_ctx->srcH;
    if (dx < 0 || dy < 0 || dx >= display_width() || dy >= display_height()) return;
    display_draw_pixel(dx, dy, gray4);
}

static int jpegDrawCallback(JPEGDRAW* pDraw) {
    if (!g_ctx || !pDraw) return 0;
    for (int yy = 0; yy < pDraw->iHeight; yy++) {
        for (int xx = 0; xx < pDraw->iWidth; xx++) {
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            plot_scaled(pDraw->x + xx, pDraw->y + yy, rgb565_to_gray4(px));
        }
    }
    return 1;
}

static int pngDrawCallback(PNGDRAW* pDraw) {
    if (!g_ctx || !pDraw || !g_ctx->pngLineBuffer) return 0;
    g_png.getLineAsRGB565(pDraw, g_ctx->pngLineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
        plot_scaled(xx, pDraw->y, rgb565_to_gray4(g_ctx->pngLineBuffer[xx]));
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

static bool load_file(const String& path, uint8_t** outData, size_t* outSize) {
    *outData = nullptr;
    *outSize = 0;
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) return false;
    size_t size = f.size();
    if (size == 0 || size > 8 * 1024 * 1024) {
        f.close();
        return false;
    }
    uint8_t* data = (uint8_t*)ps_malloc(size);
    if (!data) {
        f.close();
        return false;
    }
    size_t read = f.read(data, size);
    f.close();
    if (read != size) {
        free(data);
        return false;
    }
    *outData = data;
    *outSize = size;
    return true;
}

static std::vector<String> list_sleep_images() {
    std::vector<String> paths;
    File dir = SD.open(SLEEP_IMAGES_DIR);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("Sleep: cannot open directory %s\n", SLEEP_IMAGES_DIR);
        return paths;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
                // entry.name() may return the full path or just the filename
                // depending on the ESP32 Arduino core version.  Always normalise
                // to SLEEP_IMAGES_DIR/filename so SD.open() works reliably.
                int lastSlash = name.lastIndexOf('/');
                String filename = (lastSlash >= 0) ? name.substring(lastSlash + 1) : name;
                String fullPath = String(SLEEP_IMAGES_DIR) + "/" + filename;
                Serial.printf("Sleep: found image %s\n", fullPath.c_str());
                paths.push_back(fullPath);
            }
        }
        entry.close();
    }
    dir.close();
    std::sort(paths.begin(), paths.end());
    Serial.printf("Sleep: %d images in %s\n", (int)paths.size(), SLEEP_IMAGES_DIR);
    return paths;
}

static bool render_image_data(const String& path, uint8_t* data, size_t size) {
    bool isJpeg = ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
    bool isPng = ends_with_ci(path, ".png");
    if (!isJpeg && !isPng) return false;

    SleepDrawContext ctx;
    bool ok = false;

    if (isJpeg) {
        if (g_jpeg.openRAM(data, (int)size, jpegDrawCallback)) {
            ctx.srcW = g_jpeg.getWidth();
            ctx.srcH = g_jpeg.getHeight();
            if (ctx.srcW > 0 && ctx.srcH > 0) {
                int drawW = display_width();
                int drawH = (ctx.srcH * drawW) / ctx.srcW;
                if (drawH > display_height()) {
                    drawH = display_height();
                    drawW = (ctx.srcW * drawH) / ctx.srcH;
                }
                ctx.dstW = std::max(1, drawW);
                ctx.dstH = std::max(1, drawH);
                ctx.dstX = (display_width() - ctx.dstW) / 2;
                ctx.dstY = (display_height() - ctx.dstH) / 2;
                g_ctx = &ctx;
                ok = g_jpeg.decode(0, 0, 0) == 1;
                g_ctx = nullptr;
            }
            g_jpeg.close();
        }
    } else {
        if (g_png.openRAM(data, (int)size, pngDrawCallback) == PNG_SUCCESS) {
            ctx.srcW = g_png.getWidth();
            ctx.srcH = g_png.getHeight();
            if (ctx.srcW > 0 && ctx.srcH > 0) {
                int drawW = display_width();
                int drawH = (ctx.srcH * drawW) / ctx.srcW;
                if (drawH > display_height()) {
                    drawH = display_height();
                    drawW = (ctx.srcW * drawH) / ctx.srcH;
                }
                ctx.dstW = std::max(1, drawW);
                ctx.dstH = std::max(1, drawH);
                ctx.dstX = (display_width() - ctx.dstW) / 2;
                ctx.dstY = (display_height() - ctx.dstH) / 2;
                // Allocate line buffer from PSRAM sized for the actual image width.
                // The old static lineBuffer[1024] silently failed any PNG wider than
                // 1024 px; most real photos (1920×1080, 4000×3000, etc.) would abort.
                ctx.pngLineBuffer = (uint16_t*)ps_malloc(ctx.srcW * sizeof(uint16_t));
                if (ctx.pngLineBuffer) {
                    g_ctx = &ctx;
                    ok = g_png.decode(nullptr, 0) == PNG_SUCCESS;
                    g_ctx = nullptr;
                    free(ctx.pngLineBuffer);
                    ctx.pngLineBuffer = nullptr;
                }
            }
            g_png.close();
        }
    }

    return ok;
}

// Generate a nice sleep screen when no custom images are available.
// Draws a centered "zzz" with a soft gradient border — looks intentional
// on e-ink rather than the jarring "Sleeping..." text fallback.
static bool render_default_sleep_screen() {
    display_fill_screen(15);  // white background

    // Soft border vignette (darker edges)
    int w = display_width();
    int h = display_height();
    int border = 60;
    for (int i = 0; i < border; i++) {
        uint8_t gray = 15 - (15 - 12) * (border - i) / border;  // 12..15
        display_draw_hline(0, i, w, gray);
        display_draw_hline(0, h - 1 - i, w, gray);
        display_draw_vline(i, 0, h, gray);
        display_draw_vline(w - 1 - i, 0, h, gray);
    }

    // Centered sleep text
    const char* line1 = "z z z";
    int w1 = display_text_width(line1);
    display_draw_text((w - w1) / 2, h / 2 - 30, line1, 6);

    const char* line2 = "Sweet dreams...";
    int w2 = display_text_width(line2);
    display_draw_text((w - w2) / 2, h / 2 + 40, line2, 10);

    display_update();
    return true;
}

bool sleep_image_show_next() {
    std::vector<String> images = list_sleep_images();
    if (images.empty()) {
        Serial.println("Sleep: no images found, using default sleep screen");
        return render_default_sleep_screen();
    }

    g_prefs.begin("ereader", false);
    uint32_t idx = g_prefs.getUInt("sleepImgIdx", 0);
    g_prefs.putUInt("sleepImgIdx", (idx + 1) % images.size());
    g_prefs.end();

    // Try the chosen image first, then fall through the rest of the list so a
    // single bad/oversized file doesn't silently disable the sleep-image flow.
    for (size_t attempt = 0; attempt < images.size(); ++attempt) {
        String path = images[(idx + attempt) % images.size()];
        Serial.printf("Sleep: trying %s\n", path.c_str());
        uint8_t* data = nullptr;
        size_t size = 0;
        if (!load_file(path, &data, &size)) {
            Serial.printf("Sleep: failed to load %s\n", path.c_str());
            continue;
        }

        Serial.printf("Sleep: loaded %d bytes, rendering...\n", (int)size);
        display_fill_screen(15);
        bool ok = render_image_data(path, data, size);
        free(data);

        if (ok) {
            Serial.println("Sleep: image rendered successfully");
            display_update();
            return true;
        }
        Serial.printf("Sleep: render failed for %s\n", path.c_str());
    }

    Serial.println("Sleep: no images could be rendered");
    return false;
}
