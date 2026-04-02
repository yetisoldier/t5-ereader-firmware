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

static JPEGDEC g_jpeg;
static PNG g_png;
static Preferences g_prefs;

struct SleepDrawContext {
    int srcW = 0;
    int srcH = 0;
    int dstX = 0;
    int dstY = 0;
    int dstW = 0;
    int dstH = 0;
    uint16_t* pngLineBuffer = nullptr;
};

static SleepDrawContext* g_ctx = nullptr;

static File* openSdFileHandle(const char* path, int32_t* outSize, const char* tag) {
    if (outSize) *outSize = 0;
    File* file = new File(SD.open(path, FILE_READ));
    if (!file || !(*file) || file->isDirectory()) {
        Serial.printf("Sleep %s: open file failed for %s\n", tag, path ? path : "(null)");
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
    return openSdFileHandle(path, outSize, "JPEG");
}

static int32_t jpegFileRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
    if (!pFile || !pFile->fHandle) return 0;
    return (int32_t)((File*)pFile->fHandle)->read(pBuf, len);
}

static int32_t jpegFileSeek(JPEGFILE* pFile, int32_t position) {
    if (!pFile || !pFile->fHandle) return 0;
    return ((File*)pFile->fHandle)->seek(position) ? position : 0;
}

static void jpegFileClose(void* handle) {
    if (!handle) return;
    File* file = (File*)handle;
    file->close();
    delete file;
}

static void* pngFileOpen(const char* path, int32_t* outSize) {
    return openSdFileHandle(path, outSize, "PNG");
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

static inline uint8_t rgb565_to_gray4(uint16_t px) {
    uint8_t r = ((px >> 11) & 0x1F) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    uint8_t gray8 = (uint8_t)((r * 38 + g * 75 + b * 15) >> 7);
    return gray8 >> 4;  // 0=black, 15=white — EPD convention
}

static inline void plot_scaled(int sx, int sy, uint8_t gray4) {
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
            int sx = pDraw->x + xx;
            int sy = pDraw->y + yy;
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            plot_scaled(sx, sy, rgb565_to_gray4(px));
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

static bool open_image_file(const String& path, File* outFile, size_t* outSize) {
    if (!outFile || !outSize) return false;
    *outSize = 0;
    *outFile = SD.open(path, FILE_READ);
    if (!(*outFile) || outFile->isDirectory()) {
        Serial.printf("Sleep: SD.open failed for %s\n", path.c_str());
        return false;
    }

    *outSize = outFile->size();
    if (*outSize == 0) {
        Serial.printf("Sleep: file is empty: %s\n", path.c_str());
        outFile->close();
        return false;
    }
    if (*outSize > 8 * 1024 * 1024) {
        Serial.printf("Sleep: file too large (%u bytes): %s\n", (unsigned)*outSize, path.c_str());
        outFile->close();
        return false;
    }
    return true;
}

static void append_sleep_images_from_dir(std::vector<String>& paths, const char* dirPath) {
    Serial.printf("Sleep: scanning directory %s\n", dirPath);
    File dir = SD.open(dirPath);
    if (!dir) {
        Serial.printf("Sleep: directory missing or unopenable: %s\n", dirPath);
        return;
    }
    if (!dir.isDirectory()) {
        Serial.printf("Sleep: path is not a directory: %s\n", dirPath);
        dir.close();
        return;
    }

    bool foundAnyEntry = false;
    bool foundAnyCandidate = false;
    File entry;
    while ((entry = dir.openNextFile())) {
        foundAnyEntry = true;
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
                foundAnyCandidate = true;
                // entry.name() may return the full path or just the filename
                // depending on the ESP32 Arduino core version. Always normalise
                // to dirPath/filename so SD.open() works reliably.
                int lastSlash = name.lastIndexOf('/');
                String filename = (lastSlash >= 0) ? name.substring(lastSlash + 1) : name;
                String fullPath = String(dirPath) + "/" + filename;
                Serial.printf("Sleep: candidate image %s\n", fullPath.c_str());
                paths.push_back(fullPath);
            } else {
                Serial.printf("Sleep: skipping non-image file %s\n", name.c_str());
            }
        }
        entry.close();
    }

    if (!foundAnyEntry) {
        Serial.printf("Sleep: directory exists but is empty: %s\n", dirPath);
    } else if (!foundAnyCandidate) {
        Serial.printf("Sleep: directory contains entries but no supported images: %s\n", dirPath);
    }
    dir.close();
}

static std::vector<String> list_sleep_images() {
    std::vector<String> paths;

    if (SD.cardType() == CARD_NONE) {
        Serial.println("Sleep: SD card mount failure, cannot discover sleep images");
        return paths;
    }

    // Load sleep images only from the configured SD-card sleep directory.
    append_sleep_images_from_dir(paths, SLEEP_IMAGES_DIR);

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    Serial.printf("Sleep: %d total image candidates from %s\n", (int)paths.size(), SLEEP_IMAGES_DIR);
    return paths;
}

static bool render_image_file(const String& path, File& file, size_t size) {
    bool isJpeg = ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
    bool isPng = ends_with_ci(path, ".png");
    if (!isJpeg && !isPng) return false;

    bool ok = false;
    SleepDrawContext ctx;

    if (isJpeg) {
        (void)file;
        if (!g_jpeg.open(path.c_str(), jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, jpegDrawCallback)) {
            Serial.printf("Sleep JPEG: open failed for %s (err=%d)\n",
                          path.c_str(), g_jpeg.getLastError());
            return false;
        }
        ctx.srcW = g_jpeg.getWidth();
        ctx.srcH = g_jpeg.getHeight();
        int w = ctx.srcW;
        int h = ctx.srcH;
        Serial.printf("Sleep JPEG: opened %s (%u bytes, %dx%d)\n",
                      path.c_str(), (unsigned)size, w, h);
        if (w <= 0 || h <= 0) {
            Serial.printf("Sleep JPEG: invalid dimensions for %s\n", path.c_str());
            g_jpeg.close();
            return false;
        }

        int drawW = display_width();
        int drawH = (h * drawW) / w;
        if (drawH > display_height()) {
            drawH = display_height();
            drawW = (w * drawH) / h;
        }
        ctx.dstW = std::max(1, drawW);
        ctx.dstH = std::max(1, drawH);
        ctx.dstX = (display_width() - ctx.dstW) / 2;
        ctx.dstY = (display_height() - ctx.dstH) / 2;

        g_ctx = &ctx;

        ok = g_jpeg.decode(0, 0, 0) == 1;
        g_ctx = nullptr;
        Serial.printf("Sleep JPEG: decode %s for %s (err=%d)\n",
                      ok ? "OK" : "FAILED", path.c_str(), g_jpeg.getLastError());
        g_jpeg.close();
    } else {
        if (g_png.open(path.c_str(), pngFileOpen, pngFileClose, pngFileRead, pngFileSeek, pngDrawCallback) != PNG_SUCCESS) {
            Serial.printf("Sleep PNG: open failed for %s (err=%d)\n",
                          path.c_str(), g_png.getLastError());
            return false;
        }
        ctx.srcW = g_png.getWidth();
        ctx.srcH = g_png.getHeight();
        int w = ctx.srcW;
        int h = ctx.srcH;
        Serial.printf("Sleep PNG: opened %s (%u bytes, %dx%d)\n",
                      path.c_str(), (unsigned)size, w, h);
        if (w <= 0 || h <= 0) {
            Serial.printf("Sleep PNG: invalid dimensions for %s\n", path.c_str());
            g_png.close();
            return false;
        }

        int drawW = display_width();
        int drawH = (h * drawW) / w;
        if (drawH > display_height()) {
            drawH = display_height();
            drawW = (w * drawH) / h;
        }
        ctx.dstW = std::max(1, drawW);
        ctx.dstH = std::max(1, drawH);
        ctx.dstX = (display_width() - ctx.dstW) / 2;
        ctx.dstY = (display_height() - ctx.dstH) / 2;

        ctx.pngLineBuffer = (uint16_t*)ps_malloc(w * sizeof(uint16_t));
        if (!ctx.pngLineBuffer) {
            Serial.printf("Sleep PNG: line buffer alloc failed (%d bytes) for %s\n",
                          (int)(w * sizeof(uint16_t)), path.c_str());
            g_png.close();
            return false;
        }

        g_ctx = &ctx;
        ok = g_png.decode(nullptr, 0) == PNG_SUCCESS;
        g_ctx = nullptr;
        Serial.printf("Sleep PNG: decode %s for %s (err=%d)\n",
                      ok ? "OK" : "FAILED", path.c_str(), g_png.getLastError());
        free(ctx.pngLineBuffer);
        ctx.pngLineBuffer = nullptr;
        g_png.close();
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

    display_update_sleep();
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
        File file;
        size_t size = 0;
        if (!open_image_file(path, &file, &size)) {
            Serial.printf("Sleep: failed to open %s\n", path.c_str());
            continue;
        }

        Serial.printf("Sleep: opened %u bytes, rendering...\n", (unsigned)size);
        display_fill_screen(15);
        bool ok = render_image_file(path, file, size);
        file.close();

        if (ok) {
            Serial.printf("Sleep: rendered %s\n", path.c_str());
            display_update_sleep();
            return true;
        }
        Serial.printf("Sleep: render failed for %s\n", path.c_str());
    }

    Serial.println("Sleep: no valid images, using default sleep screen");
    return render_default_sleep_screen();
}
