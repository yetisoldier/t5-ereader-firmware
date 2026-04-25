#include "ui_settings.h"
#include "config.h"
#include "../settings.h"
#include "../display.h"
#include "../battery.h"
#include "../reader.h"
#include "../cover_renderer.h"

// ─── Extern declarations for shared helpers in main.cpp ─────────────
extern void drawHeader(const char* title, bool showBattery = true);
extern void drawBottomBar(const char* label);

// ─── Layout constants ───────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;
static const int SETTINGS_ROW_H = FONT_H + 8;
static int settingsPage = 0; // 0 = Reading, 1 = Device

// ─── Settings option arrays ─────────────────────────────────────────
static const char* fontSizeNames[] = {"XS", "S", "M", "M-L", "L"};
static const int NUM_FONT_SIZES = 5;
static const char* lineSpacingNames[] = {"Compact", "Normal", "Relaxed", "Spacious", "Extra"};
static const char* libraryViewNames[] = {"List", "Poster"};
static const char* librarySortNames[] = {"Title", "Author", "Recent", "Size"};
static const int sleepOptions[] = {2, 5, 10, 15, 30};
static const int NUM_SLEEP_OPTIONS = 5;
static const int refreshOptions[] = {1, 2, 4, 6, 10};
static const int NUM_REFRESH_OPTIONS = 5;

// ─── Helpers ────────────────────────────────────────────────────────

static int findSleepIdx() {
    int val = settings_get().sleepTimeoutMin;
    for (int i = 0; i < NUM_SLEEP_OPTIONS; i++) {
        if (sleepOptions[i] == val) return i;
    }
    return 1;
}

static int findRefreshIdx() {
    int val = settings_get().refreshEveryPages;
    for (int i = 0; i < NUM_REFRESH_OPTIONS; i++) {
        if (refreshOptions[i] == val) return i;
    }
    return 1;
}

// ═══════════════════════════════════════════════════════════════════
// Settings screen drawing
// ═══════════════════════════════════════════════════════════════════

void ui_settings_draw(bool& settingsFromLibrary) {
    display_fill_screen(15);
    drawHeader(settingsPage == 0 ? "Settings: Reading" : "Settings: Device");

    Settings& s = settings_get();
    int y = HEADER_HEIGHT + MARGIN_Y + 10;
    const int labelX = MARGIN_X;

    if (settingsPage == 0) {
        // Font Size — draw label in UI font, then preview in selected font
        display_draw_text(labelX, y + FONT_H - 4, "Font Size", 0);
        char fsLabel[32];
        snprintf(fsLabel, sizeof(fsLabel), "< %s >", fontSizeNames[s.fontSizeLevel]);
        int fsw = display_text_width(fsLabel);
        display_draw_text(W - MARGIN_X - fsw, y + FONT_H - 4, fsLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Font Family (serif/sans toggle)
        display_draw_text(labelX, y + FONT_H - 4, "Font Family", 0);
        const char* familyLabel = s.serifFont ? "< Serif >" : "< Sans >";
        int ffw = display_text_width(familyLabel);
        display_draw_text(W - MARGIN_X - ffw, y + FONT_H - 4, familyLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Line spacing
        display_draw_text(labelX, y + FONT_H - 4, "Line Spacing", 0);
        char lsLabel[32];
        snprintf(lsLabel, sizeof(lsLabel), "< %s >", lineSpacingNames[min((int)s.lineSpacingLevel, 4)]);
        int lsw = display_text_width(lsLabel);
        display_draw_text(W - MARGIN_X - lsw, y + FONT_H - 4, lsLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Font preview line in the selected font size + family
        display_set_font(s.fontSizeLevel, s.serifFont);
        const char* preview = "The quick brown fox jumps over the lazy dog";
        display_draw_text(labelX, y + display_font_height() - 4, preview, 6);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;
        display_set_font_size(2);  // back to UI font

        // Sleep Timeout
        display_draw_text(labelX, y + FONT_H - 4, "Sleep Timeout", 0);
        char slLabel[32];
        snprintf(slLabel, sizeof(slLabel), "< %dm >", s.sleepTimeoutMin);
        int slw = display_text_width(slLabel);
        display_draw_text(W - MARGIN_X - slw, y + FONT_H - 4, slLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Sleep now
        display_draw_text(labelX, y + FONT_H - 4, "Sleep Now", 0);
        const char* snLabel = "[ Sleep ]";
        int snw = display_text_width(snLabel);
        display_draw_text(W - MARGIN_X - snw, y + FONT_H - 4, snLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Cleanup refresh cadence
        display_draw_text(labelX, y + FONT_H - 4, "Cleanup Refresh", 0);
        char rfLabel[32];
        snprintf(rfLabel, sizeof(rfLabel), "< %d pg >", s.refreshEveryPages);
        int rfw = display_text_width(rfLabel);
        display_draw_text(W - MARGIN_X - rfw, y + FONT_H - 4, rfLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Page Numbers
        display_draw_text(labelX, y + FONT_H - 4, "Page Numbers", 0);
        const char* pnLabel = s.showPageNumbers ? "[ ON ]" : "[ OFF ]";
        int pnw = display_text_width(pnLabel);
        display_draw_text(W - MARGIN_X - pnw, y + FONT_H - 4, pnLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;
    } else {
        // Library view mode
        display_draw_text(labelX, y + FONT_H - 4, "Library View", 0);
        char lvLabel[32];
        snprintf(lvLabel, sizeof(lvLabel), "< %s >", libraryViewNames[s.libraryViewMode]);
        int lvw = display_text_width(lvLabel);
        display_draw_text(W - MARGIN_X - lvw, y + FONT_H - 4, lvLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Library sort
        display_draw_text(labelX, y + FONT_H - 4, "Library Sort", 0);
        char sortLabel[32];
        snprintf(sortLabel, sizeof(sortLabel), "< %s >", librarySortNames[min((int)s.librarySortOrder, 3)]);
        int sortW = display_text_width(sortLabel);
        display_draw_text(W - MARGIN_X - sortW, y + FONT_H - 4, sortLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // WiFi Upload
        display_draw_text(labelX, y + FONT_H - 4, "WiFi Upload", 0);
        const char* wuLabel = "[ Open ]";
        int wuw = display_text_width(wuLabel);
        display_draw_text(W - MARGIN_X - wuw, y + FONT_H - 4, wuLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Poster Covers
        display_draw_text(labelX, y + FONT_H - 4, "Poster Covers", 0);
        const char* pcLabel = s.posterShowCovers ? "[ ON ]" : "[ OFF ]";
        int pcw = display_text_width(pcLabel);
        display_draw_text(W - MARGIN_X - pcw, y + FONT_H - 4, pcLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // WiFi SSID (display only)
        display_draw_text(labelX, y + FONT_H - 4, "WiFi SSID", 0);
        String ssid = s.wifiSSID;
        if (ssid.length() > 15) ssid = ssid.substring(0, 12) + "...";
        int sw = display_text_width(ssid.c_str());
        display_draw_text(W - MARGIN_X - sw, y + FONT_H - 4, ssid.c_str(), 6);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Battery Display
        display_draw_text(labelX, y + FONT_H - 4, "Battery Display", 0);
        const char* bdLabel = s.showBattery ? "[ ON ]" : "[ OFF ]";
        int bdw = display_text_width(bdLabel);
        display_draw_text(W - MARGIN_X - bdw, y + FONT_H - 4, bdLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;

        // Check for Update
        display_draw_text(labelX, y + FONT_H - 4, "Firmware Update", 0);
        const char* otaLabel = "[ Check ]";
        int otaw = display_text_width(otaLabel);
        display_draw_text(W - MARGIN_X - otaw, y + FONT_H - 4, otaLabel, 3);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;
    }

    // Lower gap: page navigation + reset/version in lower gap
    {
        int footerTop = H - FOOTER_HEIGHT;
        int navY = y + 50;
        
        // Draw tab buttons similar to library page controls
        // Active tab has filled background, inactive tab is gray text
        const char* leftLabel = settingsPage == 0 ? "Reading" : "< Reading";
        const char* rightLabel = settingsPage == 0 ? "Device >" : "Device";
        
        // Left tab (Reading)
        int leftW = display_text_width(leftLabel) + 20;
        if (settingsPage == 0) {
            // Active: outline-only box
            display_draw_rect(MARGIN_X, navY - 5, leftW, FONT_H + 10, 0);
            display_draw_text(MARGIN_X + 10, navY + FONT_H - 4, leftLabel, 0);
        } else {
            // Inactive: gray text
            display_draw_text(MARGIN_X, navY + FONT_H - 4, leftLabel, 3);
        }
        
        // Right tab (Device)
        int rightW = display_text_width(rightLabel) + 20;
        if (settingsPage == 1) {
            // Active: outline-only box
            display_draw_rect(W - MARGIN_X - rightW, navY - 5, rightW, FONT_H + 10, 0);
            display_draw_text(W - MARGIN_X - rightW + 10, navY + FONT_H - 4, rightLabel, 0);
        } else {
            // Inactive: gray text
            int rtw = display_text_width(rightLabel);
            display_draw_text(W - MARGIN_X - rtw, navY + FONT_H - 4, rightLabel, 3);
        }

        int baseY = navY + 90;
        if (baseY + FONT_H < footerTop - 20) {
            const char* resetLabel = "[ Reset Defaults ]";
            display_draw_text(MARGIN_X, baseY, resetLabel, 3);

            char verLabel[32];
            if (FIRMWARE_VERSION[0] == 'v' || FIRMWARE_VERSION[0] == 'V') {
                snprintf(verLabel, sizeof(verLabel), "Firmware: %s", FIRMWARE_VERSION);
            } else {
                snprintf(verLabel, sizeof(verLabel), "Firmware: v%s", FIRMWARE_VERSION);
            }
            int vw = display_text_width(verLabel);
            display_draw_text(W - MARGIN_X - vw, baseY, verLabel, 8);
        }
    }

    drawBottomBar("[ Back ]");
    display_update_medium();
}

// ═══════════════════════════════════════════════════════════════════
// Settings touch handling
// ═══════════════════════════════════════════════════════════════════

AppState ui_settings_touch(int x, int y, BookReader& reader, void (*enterSleepCb)()) {
    // Footer → back
    if (y > H - FOOTER_HEIGHT) {
        settings_save();
        // If we were reading a book, go back to reader, otherwise library
        if (reader.getTitle().length() > 0) {
            int savedChapter = reader.getCurrentChapter();
            int savedPage = reader.getCurrentPage();
            reader.recalculateLayout();
            reader.jumpToChapter(savedChapter);
            reader.restorePage(savedPage);
            return STATE_READER;
        } else {
            return STATE_LIBRARY;
        }
    }

    Settings& s = settings_get();
    int rowY = HEADER_HEIGHT + MARGIN_Y + 10;

    // Determine which row was tapped
    int row = (y - rowY) / SETTINGS_ROW_H;
    bool rightSide = (x > W / 2);
    int rowCount = settingsPage == 0 ? 8 : 7;

    if (row >= 0 && row < rowCount) {
        if (settingsPage == 0) {
            switch (row) {
                case 0: // Font Size
                    if (rightSide) {
                        s.fontSizeLevel = (s.fontSizeLevel + 1) % NUM_FONT_SIZES;
                    } else {
                        s.fontSizeLevel = (s.fontSizeLevel + NUM_FONT_SIZES - 1) % NUM_FONT_SIZES;
                    }
                    display_set_font(s.fontSizeLevel, s.serifFont);
                    break;
                case 1: // Font Family
                    s.serifFont = !s.serifFont;
                    display_set_font(s.fontSizeLevel, s.serifFont);
                    break;
                case 2: // Line spacing
                    if (rightSide) {
                        s.lineSpacingLevel = (s.lineSpacingLevel + 1) % LINE_SPACING_LEVEL_COUNT;
                    } else {
                        s.lineSpacingLevel = (s.lineSpacingLevel + LINE_SPACING_LEVEL_COUNT - 1) % LINE_SPACING_LEVEL_COUNT;
                    }
                    break;
                case 3: // Font preview — no action
                    break;
                case 4: { // Sleep Timeout
                    int idx = findSleepIdx();
                    if (rightSide) {
                        idx = (idx + 1) % NUM_SLEEP_OPTIONS;
                    } else {
                        idx = (idx + NUM_SLEEP_OPTIONS - 1) % NUM_SLEEP_OPTIONS;
                    }
                    s.sleepTimeoutMin = sleepOptions[idx];
                    break;
                }
                case 5: // Sleep now
                    if (enterSleepCb) enterSleepCb();
                    return STATE_SETTINGS;
                case 6: { // Cleanup Refresh
                    int idx = findRefreshIdx();
                    if (rightSide) {
                        idx = (idx + 1) % NUM_REFRESH_OPTIONS;
                    } else {
                        idx = (idx + NUM_REFRESH_OPTIONS - 1) % NUM_REFRESH_OPTIONS;
                    }
                    s.refreshEveryPages = refreshOptions[idx];
                    break;
                }
                case 7: // Page Numbers
                    s.showPageNumbers = !s.showPageNumbers;
                    break;
            }
        } else {
            switch (row) {
                case 0: // Library View
                    s.libraryViewMode = (s.libraryViewMode + 1) % 2;
                    if (s.libraryViewMode == 1) {
                        s.posterShowCovers = true;
                    }
                    cover_cache_clear();
                    break;
                case 1: // Library Sort
                    if (rightSide) {
                        s.librarySortOrder = (s.librarySortOrder + 1) % 4;
                    } else {
                        s.librarySortOrder = (s.librarySortOrder + 3) % 4;
                    }
                    break;
                case 2: // WiFi Upload
                    return STATE_WIFI;
                case 3: // Poster Covers toggle
                    s.posterShowCovers = !s.posterShowCovers;
                    cover_cache_clear();
                    break;
                case 4: // WiFi SSID (display only)
                    break;
                case 5: // Battery Display
                    s.showBattery = !s.showBattery;
                    break;
                case 6: // Firmware Update
                    return STATE_OTA_CHECK;
            }
        }
        return STATE_SETTINGS;
    }

    // Lower gap: page navigation / reset defaults
    int gapTop = rowY + SETTINGS_ROW_H * rowCount;
    int footerTop = H - FOOTER_HEIGHT;
    
    // Debug: show touch coordinates
    Serial.printf("Settings touch: y=%d, gapTop=%d, footerTop=%d\n", y, gapTop, footerTop);
    
    if (y >= gapTop && y < footerTop) {
        int navY = gapTop + 50;  // Match drawing position
        int resetY = navY + 90;
        Serial.printf("  navY=%d, resetY=%d\n", navY, resetY);
        
        // Touch zone for tab buttons (wider to match visual buttons)
        if (y >= navY - 10 && y < navY + FONT_H + 10) {
            if (x < W / 2) {
                settingsPage = 0;
                Serial.println("  -> Switched to Reading page");
            } else {
                settingsPage = 1;
                Serial.println("  -> Switched to Device page");
            }
            return STATE_SETTINGS;
        }
        if (y >= resetY && y < resetY + 60 && x < W / 2) {
            settings_set_default();
            settingsPage = 0;
        }
    }

    return STATE_SETTINGS;
}
