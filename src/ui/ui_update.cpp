#include "ui_update.h"
#include "config.h"
#include "../settings.h"
#include "../display.h"
#include "../wifi_upload.h"
#include "../library.h"
#include "../cover_renderer.h"
#include "ota_update.h"
#include <WiFi.h>
#include <qrcode.h>
#include <functional>

// ─── Extern declarations for shared helpers in main.cpp ─────────────
extern void drawHeader(const char* title, bool showBattery = true);
extern void drawBottomBar(const char* label);

// ─── Layout constants ───────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;

// ═══════════════════════════════════════════════════════════════════
// OTA Update screen
// ═══════════════════════════════════════════════════════════════════

void ui_ota_draw(OtaState& otaState) {
    display_fill_screen(15);
    drawHeader("Firmware Update");

    int cy = H / 2 - 40;

    switch (otaState.phase) {
        case OTA_CHECKING: {
            const char* msg = "Checking for updates...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);
            drawBottomBar("[ Cancel ]");
            display_update_fast();
            break;
        }

        case OTA_RESULT: {
            if (otaState.updateAvailable) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s available", otaState.latestVersion.c_str());
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy, msg, 0);

                const char* tap = "Tap to install";
                int tw = display_text_width(tap);
                display_draw_text((W - tw) / 2, cy + FONT_H + 16, tap, 3);
                drawBottomBar("[ Back ]");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Up to date (v%s)", FIRMWARE_VERSION);
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy, msg, 0);
                drawBottomBar("[ Back ]");
            }
            display_update_fast();
            break;
        }

        case OTA_DOWNLOADING: {
            const char* msg = "Downloading update...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy - 20, msg, 0);

            int barX = MARGIN_X + 20;
            int barW = W - barX * 2;
            int barY = cy + 30;
            int barH = 16;
            display_draw_rect(barX, barY, barW, barH, 0);
            int fillW = (barW - 4) * otaState.progress / 100;
            if (fillW > 0) {
                display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
            }

            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%d%%", otaState.progress);
            int pw = display_text_width(pctStr);
            display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

            display_update_fast();
            break;
        }

        case OTA_DONE: {
            const char* msg = "Update complete!";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);

            const char* msg2 = "Restarting...";
            int m2w = display_text_width(msg2);
            display_draw_text((W - m2w) / 2, cy + FONT_H + 16, msg2, 3);

            display_update();

            delay(2000);
            esp_restart();
            break;
        }

        case OTA_FAILED: {
            const char* msg = "Update failed";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);

            if (otaState.latestVersion.length() > 0) {
                int lw = display_text_width(otaState.latestVersion.c_str());
                display_draw_text((W - lw) / 2, cy + FONT_H + 16, otaState.latestVersion.c_str(), 6);
            }

            drawBottomBar("[ Back ]");
            display_update_fast();
            break;
        }

        default:
            break;
    }
}

void ui_ota_tick(OtaState& otaState) {
    if (otaState.phase != OTA_CHECKING) return;

    if (WiFi.status() != WL_CONNECTED) {
        Settings& s = settings_get();
        WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(100);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        otaState.phase = OTA_FAILED;
        otaState.latestVersion = "Connect to WiFi first";
        return;
    }

    otaState.updateAvailable = ota_check_for_update(otaState.latestVersion);
    otaState.phase = OTA_RESULT;
}

AppState ui_ota_touch(int x, int y, OtaState& otaState) {
    (void)x;

    // Footer → back (cancel)
    if (y > H - FOOTER_HEIGHT) {
        otaState.phase = OTA_IDLE;
        return STATE_SETTINGS;
    }

    // In result state with update available → start download
    if (otaState.phase == OTA_RESULT && otaState.updateAvailable) {
        otaState.phase = OTA_DOWNLOADING;
        otaState.progress = 0;

        // Draw the initial download screen
        ui_ota_draw(otaState);

        // Perform the download (blocking with progress callbacks)
        bool ok = ota_install_update([&otaState](int pct) {
            otaState.progress = pct;
            if (pct % 5 == 0) {
                display_fill_screen(15);
                drawHeader("Firmware Update");

                int cy = H / 2 - 40;
                const char* msg = "Downloading update...";
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy - 20, msg, 0);

                int barX = MARGIN_X + 20;
                int barW = W - barX * 2;
                int barY = cy + 30;
                int barH = 16;
                display_draw_rect(barX, barY, barW, barH, 0);
                int fillW = (barW - 4) * pct / 100;
                if (fillW > 0) {
                    display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
                }

                char pctStr[16];
                snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
                int pw = display_text_width(pctStr);
                display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

                display_update_fast();
            }
        });

        if (ok) {
            otaState.phase = OTA_DONE;
        } else {
            otaState.phase = OTA_FAILED;
            otaState.latestVersion = "Try again later";
        }
        return STATE_OTA_CHECK;
    }

    // In failed state → back
    if (otaState.phase == OTA_FAILED || (otaState.phase == OTA_RESULT && !otaState.updateAvailable)) {
        otaState.phase = OTA_IDLE;
        return STATE_SETTINGS;
    }

    return STATE_OTA_CHECK;
}

// ═══════════════════════════════════════════════════════════════════
// WiFi Upload screen
// ═══════════════════════════════════════════════════════════════════

static void drawQrCode(const char* text, int cx, int cy, int moduleSize) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, text);

    int qrSize = qrcode.size * moduleSize;
    int startX = cx - qrSize / 2;
    int startY = cy - qrSize / 2;

    display_draw_filled_rect(startX - 8, startY - 8, qrSize + 16, qrSize + 16, 15);

    for (uint8_t row = 0; row < qrcode.size; row++) {
        for (uint8_t col = 0; col < qrcode.size; col++) {
            if (qrcode_getModule(&qrcode, col, row)) {
                display_draw_filled_rect(startX + col * moduleSize,
                                          startY + row * moduleSize,
                                          moduleSize, moduleSize, 0);
            }
        }
    }
}

void ui_wifi_draw() {
    display_fill_screen(15);
    drawHeader("WiFi Upload");

    int y = HEADER_HEIGHT + 40;

    if (wifi_upload_running()) {
        const char* l1 = "WiFi connected";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 0);
        y += FONT_H + 12;

        String ipStr = "http://" + wifi_upload_ip();
        int w2 = display_text_width(ipStr.c_str());
        display_draw_text((W - w2) / 2, y, ipStr.c_str(), 0);
        y += FONT_H + 12;

        const char* l3 = "Scan to open upload page";
        int w3 = display_text_width(l3);
        display_draw_text((W - w3) / 2, y, l3, 6);
        y += FONT_H + 20;

        drawQrCode(ipStr.c_str(), W / 2, y + 120, 5);
    } else {
        const char* l1 = "Connecting to WiFi...";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 4);
    }

    const char* hint = "Tap anywhere to return";
    int hw = display_text_width(hint);
    display_draw_text((W - hw) / 2, H - 100, hint, 10);

    display_update_fast();
}

AppState ui_wifi_touch(int x, int y,
                       std::vector<BookInfo>& books,
                       std::vector<int>& filteredIndices) {
    (void)x; (void)y;
    wifi_upload_stop();
    books = library_scan();
    // filteredIndices will be updated by caller with current filter
    cover_cache_clear();
    return STATE_LIBRARY;
}
