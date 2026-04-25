#include <Arduino.h>
#include "config.h"
#include "settings.h"
#include "display.h"
#include "touch.h"
#include "battery.h"
#include "library.h"
#include "reader.h"
#include "cover_renderer.h"
#include "sleep_image.h"
#include "wifi_upload.h"
#include "inline_image.h"
#include "ota_update.h"
#include "gnome_splash.h"
#include "opds_store.h"
#include "state.h"
#include "debug_trace.h"
#include "ui/ui_library.h"
#include "ui/ui_reader.h"
#include "ui/ui_settings.h"
#include "ui/ui_update.h"
#include <WiFi.h>
#include <Preferences.h>
#include <qrcode.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>

// ─── App State ──────────────────────────────────────────────────────
static AppState       appState = STATE_BOOT;
static bool           firstLibraryDraw = true;  // full refresh after splash to clear ghost
static bool           settingsFromLibrary = false;  // quicker refresh on library→settings transition
static unsigned long  lastActivity = 0;
static bool           needsRedraw = true;
static unsigned long  bootTime = 0;          // millis() at setup() end
static bool           lightSleepEnabled = false; // TEMPORARILY DISABLED - debugging sleep issue
static ReaderRefreshState readerRefresh = {false, false, false, 0};

// ─── Library state ──────────────────────────────────────────────────
static std::vector<BookInfo> books;
static int libraryScroll = 0;
static LibraryFilter libraryFilter = FILTER_ALL;
static std::vector<int> filteredIndices;  // indices into books[] matching current filter

// ─── Reader state ───────────────────────────────────────────────────
BookReader reader;

// ─── UI module callbacks ─────────────────────────────────────────────
void setAppState(AppState state) { appState = state; }
void setNeedsRedraw(bool val) { needsRedraw = val; }
void setReaderFastRefresh(bool val) { readerRefresh.fastRefresh = val; }
void resetReaderPageTurns() { readerRefresh.pageTurnsSinceFull = 0; }

// ─── TOC / Bookmarks scroll ─────────────────────────────────────────
static int tocScroll = 0;
static int bmScroll = 0;

// ─── Long-press tracking ────────────────────────────────────────────
static unsigned long touchDownTime = 0;
static bool touchHandled = false;
static const unsigned long LONG_PRESS_MS = 800;

// ─── Top button (GPIO 21): single=next, double=prev, long=sleep ────
static unsigned long btnDownTime = 0;
static bool btnWasPressed = false;
static unsigned long lastBtnReleaseTime = 0;
static int btnPressCount = 0;
static const unsigned long BUTTON_DEBOUNCE_MS = 50;
static const unsigned long BUTTON_POWER_MS = 600;   // hold 600ms to sleep
static const unsigned long DOUBLE_PRESS_WINDOW_MS = 400;
static unsigned long wakeCooldownEnd = 0;  // No-sleep period after wake

// ─── OTA state ──────────────────────────────────────────────────────
static OtaState otaState = {OTA_IDLE, "", false, 0};

// ─── Layout helpers ─────────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;  // UI font height (medium FiraSans advance_y)

// Book list item height
static const int BOOK_ITEM_H = FONT_H * 2 + 12;  // title + info + padding


static void showWakeFeedback() {
    const int bannerH = 110;
    const int y = H - bannerH;

    display_draw_filled_rect(0, y, W, bannerH, 15);
    display_draw_hline(0, y, W, 10);

    const char* line1 = "Waking...";
    int w1 = display_text_width(line1);
    display_draw_text((W - w1) / 2, y + 42, line1, 2);

    display_draw_filled_rect(80, y + 76, W - 160, 8, 12);
    display_draw_filled_rect(80, y + 76, (W - 160) / 3, 8, 4);

    // Update only the banner so the sleep image remains visible elsewhere,
    // giving immediate confirmation that the wake button press was accepted.
    display_update_reader_body(0, y, W, bannerH, false);
}

static void enterDeepSleep(bool triggeredByButton = false);

// ═══════════════════════════════════════════════════════════════════
// Drawing helpers
// ═══════════════════════════════════════════════════════════════════

static const char* display_version_text() {
    static char verBuf[32];
    if (FIRMWARE_VERSION[0] == 'v' || FIRMWARE_VERSION[0] == 'V') {
        snprintf(verBuf, sizeof(verBuf), "%s", FIRMWARE_VERSION);
    } else {
        snprintf(verBuf, sizeof(verBuf), "v%s", FIRMWARE_VERSION);
    }
    return verBuf;
}

void drawHeader(const char* title, bool showBattery = true) {
    display_draw_filled_rect(0, 0, W, HEADER_HEIGHT, 2);
    display_draw_text(MARGIN_X, HEADER_HEIGHT - 18, title, 15);

    if (showBattery && settings_get().showBattery) {
        char battStr[16];
        snprintf(battStr, sizeof(battStr), "%d%%", battery_percent());
        int bw = display_text_width(battStr);
        display_draw_text(W - MARGIN_X - bw, HEADER_HEIGHT - 18, battStr, 15);
    }

    display_draw_hline(0, HEADER_HEIGHT, W, 0);
}

void drawBottomBar(const char* label) {
    int barY = H - FOOTER_HEIGHT;
    display_draw_filled_rect(0, barY, W, FOOTER_HEIGHT, 13);
    display_draw_hline(0, barY, W, 8);
    int tw = display_text_width(label);
    display_draw_text((W - tw) / 2, barY + FOOTER_HEIGHT - 12, label, 3);
}

// Split bottom bar: two buttons
static void drawBottomBarSplit(const char* left, const char* right) {
    int barY = H - FOOTER_HEIGHT;
    display_draw_filled_rect(0, barY, W, FOOTER_HEIGHT, 13);
    display_draw_hline(0, barY, W, 8);
    // Vertical divider
    display_draw_filled_rect(W / 2 - 1, barY + 4, 2, FOOTER_HEIGHT - 8, 8);
    // Left label
    int lw = display_text_width(left);
    display_draw_text(W / 4 - lw / 2, barY + FOOTER_HEIGHT - 12, left, 3);
    // Right label
    int rw = display_text_width(right);
    display_draw_text(W * 3 / 4 - rw / 2, barY + FOOTER_HEIGHT - 12, right, 3);
}

static void drawGnomeSplash(const char* statusMsg = "Starting up...") {
    display_fill_screen(15);

    for (int y = 0; y < SPLASH_ART_HEIGHT; ++y) {
        int rowOffset = y * ((SPLASH_WIDTH + 7) / 8);
        for (int x = 0; x < SPLASH_WIDTH; ++x) {
            uint8_t byte = pgm_read_byte(&GNOME_SPLASH_BITMAP[rowOffset + (x / 8)]);
            bool black = (byte & (0x80 >> (x & 7))) == 0;
            display_draw_pixel(x, y, black ? 0 : 15);
        }
    }

    const int frameLeft[] = {6, 7, 8};
    const int frameRight[] = {531, 532, 533};
    const int footerTop = SPLASH_ART_HEIGHT;
    const int footerBottom = H - 1;
    const int separatorY1 = footerTop + 4;
    const int separatorY2 = footerTop + 9;
    const int borderTop = separatorY2 + 1;
    const int borderHeight = footerBottom - borderTop - 7;
    const int bottomY1 = H - 8;
    const int bottomY2 = H - 3;
    const int frameWidth = frameRight[2] - frameLeft[0] + 1;

    for (int x : frameLeft) {
        display_draw_vline(x, borderTop, borderHeight, 0);
    }
    for (int x : frameRight) {
        display_draw_vline(x, borderTop, borderHeight, 0);
    }

    display_draw_hline(frameLeft[0], separatorY1, frameWidth, 0);
    display_draw_hline(frameLeft[0], separatorY2, frameWidth, 0);
    display_draw_hline(frameLeft[0], bottomY1, frameWidth, 0);
    display_draw_hline(frameLeft[0], bottomY2, frameWidth, 0);

    // Text coordinates are baseline-based. The old version baseline was set to
    // SPLASH_ART_HEIGHT + 95, which put it at y=955 with an 860px crop — only
    // a few pixels above the bottom edge, so the glyphs were effectively drawn
    // off-screen. Use a footer-specific layout and the small UI font so both
    // lines fit cleanly in the reserved splash footer.
    display_set_font_size(1);
    const int splashFontH = display_font_height();
    const int versionY = bottomY1 - 10;
    const int statusY = max(separatorY2 + splashFontH + 10, versionY - splashFontH - 10);

    int statusW = display_text_width(statusMsg);
    display_draw_text((SPLASH_WIDTH - statusW) / 2, statusY, statusMsg, 4);

    const char* verStr = display_version_text();
    int vw = display_text_width(verStr);
    display_draw_text((SPLASH_WIDTH - vw) / 2, versionY, verStr, 8);

    display_set_font_size(2);
}

// ═══════════════════════════════════════════════════════════════════
// Library screen
// ═══════════════════════════════════════════════════════════════════

static const int FILTER_TAB_H = 44;

static void updateFilteredIndices() {
    filteredIndices = library_filter(books, libraryFilter);
}

static void drawLibraryScreen() {
    ui_library_draw(books, libraryScroll, (int)libraryFilter, filteredIndices, firstLibraryDraw);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Reader screen
// ═══════════════════════════════════════════════════════════════════

static void drawReaderScreen() {
    ui_reader_draw(reader, readerRefresh);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Menu overlay (redesigned)
// ═══════════════════════════════════════════════════════════════════

static void drawMenuOverlay() {
    ui_reader_menu_draw(reader);
    needsRedraw = false;
}

static void drawGotoScreen() {
    ui_reader_goto_draw(reader);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// TOC screen
// ═══════════════════════════════════════════════════════════════════

static void drawTocScreen() {
    ui_reader_toc_draw(reader, tocScroll);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Bookmarks screen
// ═══════════════════════════════════════════════════════════════════

static void drawBookmarksScreen() {
    ui_reader_bookmarks_draw(reader, bmScroll);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Settings screen (delegated to ui/ui_settings)
// ═══════════════════════════════════════════════════════════════════

static void drawSettingsScreen() {
    ui_settings_draw(settingsFromLibrary);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// OTA Update screen (delegated to ui/ui_update)
// ═══════════════════════════════════════════════════════════════════

static void drawOtaScreen() {
    ui_ota_draw(otaState);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// WiFi Upload screen (delegated to ui/ui_update)
// ═══════════════════════════════════════════════════════════════════

static void drawWifiScreen() {
    ui_wifi_draw();
    needsRedraw = false;
}


// ═══════════════════════════════════════════════════════════════════
// Touch handlers
// ═══════════════════════════════════════════════════════════════════

static void handleLibraryTouch(int x, int y) {
    int filter = (int)libraryFilter;
    AppState newState = ui_library_touch(x, y, books, libraryScroll, filter, filteredIndices);
    libraryFilter = (LibraryFilter)filter;

    if (newState == STATE_READER) {
        // First draw after opening a book: use medium refresh for cleaner display
        readerRefresh.fastRefresh = true;
        readerRefresh.pageTurnsSinceFull = settings_get().refreshEveryPages - 1;  // triggers medium refresh
        appState = STATE_READER;
        needsRedraw = true;
        return;
    }

    if (newState == STATE_SETTINGS) {
        settingsFromLibrary = true;
        appState = STATE_SETTINGS;
        needsRedraw = true;
        return;
    }

    if (newState == STATE_OPDS_BROWSE) {
        appState = STATE_OPDS_BROWSE;
        needsRedraw = true;
        return;
    }

    needsRedraw = true;
}

static void handleReaderTouch(int x, int y, bool isLongPress) {
    appState = ui_reader_touch(x, y, isLongPress, reader, readerRefresh);
    needsRedraw = true;
}

static void handleMenuTouch(int x, int y) {
    AppState newState = ui_reader_menu_touch(x, y, reader, readerRefresh);
    if (newState == STATE_TOC) tocScroll = 0;
    if (newState == STATE_BOOKMARKS) bmScroll = 0;
    appState = newState;
    needsRedraw = true;
}

static void handleGotoTouch(int x, int y) {
    appState = ui_reader_goto_touch(x, y, reader, readerRefresh);
    needsRedraw = true;
}

static void handleTocTouch(int x, int y) {
    appState = ui_reader_toc_touch(x, y, reader, tocScroll, readerRefresh);
    needsRedraw = true;
}

static void handleBookmarksTouch(int x, int y) {
    appState = ui_reader_bookmarks_touch(x, y, reader, bmScroll, readerRefresh);
    needsRedraw = true;
}

static void handleSettingsTouch(int x, int y) {
    AppState newState = ui_settings_touch(x, y, reader, []() { enterDeepSleep(); });
    if (newState == STATE_WIFI) {
        wifi_upload_start();
        lightSleepEnabled = false;  // disable light sleep during WiFi
    }
    if (newState == STATE_OTA_CHECK) {
        lightSleepEnabled = false;  // disable light sleep during OTA
        otaState.phase = OTA_CHECKING;
        otaState.updateAvailable = false;
        otaState.latestVersion = "";
    }
    if (newState == STATE_READER) {
        readerRefresh.fastRefresh = false;
    }
    appState = newState;
    needsRedraw = true;
}

static void handleOtaTouch(int x, int y) {
    AppState newState = ui_ota_touch(x, y, otaState);
    if (newState != STATE_OTA_CHECK && newState != STATE_WIFI) {
        lightSleepEnabled = true;  // re-enable light sleep when leaving OTA
    }
    appState = newState;
    needsRedraw = true;
}

static void handleWifiTouch(int x, int y) {
    AppState newState = ui_wifi_touch(x, y, books, filteredIndices);
    updateFilteredIndices();
    if (newState != STATE_WIFI && newState != STATE_OTA_CHECK) {
        lightSleepEnabled = true;  // re-enable light sleep when leaving WiFi/OTA
    }
    appState = newState;
    needsRedraw = true;
}

// ═══════════════════════════════════════════════════════════════════
// Deep sleep
// ═══════════════════════════════════════════════════════════════════

static void enterDeepSleep(bool triggeredByButton) {
    if (Serial) Serial.println("Entering deep sleep...");

    // Save progress whenever a book is open, regardless of which sub-state
    // (reader, menu, TOC, bookmarks, settings) we're currently in.
    if (reader.getTitle().length() > 0) {
        reader.updateReadingTime();
        reader.saveProgress();
    }
    if (wifi_upload_running()) {
        wifi_upload_stop();
    }

    // Persist app state so wake can resume where we left off.
    // Reader sub-screens (menu/TOC/bookmarks) resume to reader.
    // Settings/WiFi/OTA resume to library (transient screens).
    {
        Preferences prefs;
        prefs.begin("ereader", false);

        int resumeState = (int)appState;
        // Collapse reader overlay states back to reader
        if (appState == STATE_MENU || appState == STATE_GOTO || appState == STATE_TOC || appState == STATE_BOOKMARKS) {
            resumeState = (int)STATE_READER;
        }
        // Transient states resume to library
        if (appState == STATE_WIFI || appState == STATE_OTA_CHECK || appState == STATE_SETTINGS ||
            appState == STATE_OPDS_BROWSE || appState == STATE_OPDS_DOWNLOAD) {
            resumeState = (int)STATE_LIBRARY;
        }
        prefs.putInt("sleepState", resumeState);
        prefs.putInt("sleepLibScrl", libraryScroll);

        // Persist open book filepath so we can reopen it on wake
        if (reader.getFilepath().length() > 0) {
            prefs.putString("sleepBook", reader.getFilepath());
        } else {
            prefs.putString("sleepBook", "");
        }

        // Book is stable at sleep time — reset crash guard
        prefs.putInt("crashCount", 0);

        if (Serial) {
            Serial.printf("Sleep: saved state=%d libScrl=%d book=%s\n",
                resumeState, libraryScroll, reader.getFilepath().c_str());
        }
        prefs.end();
    }

    // CRITICAL FIX: Single bounded wait for button release with defensive timeout.
    // Button is active LOW, so we wait for HIGH (released) state.
    if (triggeredByButton) {
        if (Serial) Serial.println("Sleep: waiting for button release (max 1s)...");
        unsigned long waitStart = millis();
        // Wait for HIGH (button released) with 1s timeout
        while (digitalRead(BUTTON_PIN) == LOW && (millis() - waitStart < 1000)) {
            delay(5);  // Faster polling for more responsive check
        }
        if (digitalRead(BUTTON_PIN) == LOW) {
            if (Serial) Serial.println("Sleep: button still LOW after 1s, proceeding anyway");
        } else {
            if (Serial) Serial.println("Sleep: button released");
        }
        delay(50);  // Brief settle time
    }

    // Only flush Serial if USB is connected (avoid blocking when disconnected)
    if (Serial) {
        Serial.println("Sleep: showing sleep image...");
        Serial.flush();
    }

    // Show sleep image (or default screen if no images found)
    sleep_image_show_next();
    
    if (Serial) {
        Serial.println("Sleep: display updated");
        Serial.flush();
    }

    delay(50);  // Brief settle time

    // CRITICAL: Disable GPIO wakeup source configured for light sleep.
    // If we don't clear this, the touch INT pin (GPIO 47) can trigger an
    // immediate wake because GPIO wake sources persist across sleep types.
    // This was the root cause of the "immediate restart" bug after refactoring.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // Wake when button goes LOW (active LOW, pressed)
    // ESP_EXT1_WAKEUP_ALL_LOW: wake when ALL configured pins are LOW
    // Since we only have one pin, this effectively means "wake when this pin is LOW"
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ALL_LOW);

    if (Serial) {
        Serial.println("Sleep: entering deep sleep now");
        Serial.flush();
    }
    delay(50);  // Brief settle time (reduced since we're not flushing)
    
    // Hold GPIO pins in stable state before sleep
    gpio_pullup_en((gpio_num_t)BUTTON_PIN);
    gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
    
    // Feed watchdog before deep sleep to ensure clean state
    esp_task_wdt_reset();
    
    esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════
// Light Sleep — idle power reduction (~40mA → ~2mA)
// ═══════════════════════════════════════════════════════════════════

static bool lightSleepConfigured = false;
static unsigned long lastTouchOrButtonTime = 0;  // last physical input
static const unsigned long LIGHT_SLEEP_IDLE_MS = 500; // idle before sleeping

static void configureLightSleep() {
    if (lightSleepConfigured) return;

    // TEMPORARILY DISABLED - debugging button/sleep issue
    // gpio_wakeup_enable((gpio_num_t)TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
    // esp_sleep_enable_gpio_wakeup();
    // gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);

    lightSleepConfigured = true;
    Serial.println("Light sleep DISABLED for debugging");
}

static bool canEnterLightSleep() {
    // Don't sleep during WiFi-active states
    if (appState == STATE_WIFI || appState == STATE_OTA_CHECK ||
        appState == STATE_OPDS_BROWSE || appState == STATE_OPDS_DOWNLOAD) {
        return false;
    }

    // Don't sleep if WiFi is connected (upload/OTA in progress)
    if (wifi_upload_running()) return false;

    // Don't sleep if light sleep is explicitly disabled
    if (!lightSleepEnabled) return false;

    // Don't sleep in the first 3 seconds after boot (let display settle)
    if (millis() - bootTime < 3000) return false;

    // Don't sleep if there's a pending redraw
    if (needsRedraw) return false;

    // Don't sleep if we just had input recently
    if (millis() - lastTouchOrButtonTime < LIGHT_SLEEP_IDLE_MS) return false;

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Arduino setup/loop
// ═══════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== T5 E-Reader Firmware (Portrait) ===");
    debug_trace_boot_report();
    debug_trace_mark("setup:start");

    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    bool wakingFromSleep = (wakeReason == ESP_SLEEP_WAKEUP_EXT1 ||
                            wakeReason == ESP_SLEEP_WAKEUP_TIMER);
    Serial.printf("Wakeup cause: %d (fromSleep=%d)\n", (int)wakeReason, wakingFromSleep);

    // Top button is sleep / wake; middle button is for page turns.
    // Both are active LOW.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    // PAGE_BUTTON_PIN removed — top button (GPIO 21) handles all input

    // Set wake cooldown if waking from sleep
    if (wakingFromSleep) {
        wakeCooldownEnd = millis() + 2000;  // No-sleep for 2s after wake
    }

    // Critical: wait for button release before proceeding when waking from sleep.
    // If we don't wait, the still-held button triggers immediate re-sleep.
    if (wakingFromSleep) {
        Serial.println("Wake: waiting for button release...");
        unsigned long waitStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW && millis() - waitStart < 2000) {
            delay(10);
        }
        delay(100);  // Additional debounce
        Serial.println("Wake: button released, proceeding");
    }

    debug_trace_mark("setup:before_display_init");
    display_init();
    debug_trace_mark("setup:after_display_init");

    if (wakingFromSleep) {
        showWakeFeedback();
    }

    if (!wakingFromSleep) {
        // Cold boot: show gnome splash screen.
        display_clear();
        drawGnomeSplash();
        display_update();
    }
    // Wake path: do NOT call display_clear() — the sleep image is still
    // latched on the panel.  The first drawXxxScreen() call will overwrite
    // the framebuffer and push a full refresh, which cleanly replaces the
    // sleep image with the restored UI.

    debug_trace_mark("setup:before_battery_init");
    battery_init();
    debug_trace_mark("setup:after_battery_init");
    Serial.printf("Battery: %.2fV (%d%%)\n", battery_voltage(), battery_percent());

    debug_trace_mark("setup:before_touch_init");
    if (!touch_init()) {
        Serial.println("WARNING: Touch not available");
    }
    debug_trace_mark("setup:after_touch_init");

    debug_trace_mark("setup:before_library_init");
    if (!library_init()) {
        display_fill_screen(15);
        const char* err = "SD Card Error!";
        int ew = display_text_width(err);
        display_draw_text((W - ew) / 2, H / 2 - 20, err, 0);
        const char* hint = "Insert SD card and restart";
        int hw = display_text_width(hint);
        display_draw_text((W - hw) / 2, H / 2 + 40, hint, 6);
        display_update();
    }

    debug_trace_mark("setup:after_library_init");
    settings_init();
    debug_trace_mark("setup:after_settings_init");
    display_set_font(settings_get().fontSizeLevel, settings_get().serifFont);
    debug_trace_mark("setup:before_library_scan");
    books = library_scan();
    debug_trace_mark("setup:after_library_scan", String(books.size()));
    updateFilteredIndices();
    cover_cache_clear();
    wifi_upload_init();
    wifi_upload_set_reader(&reader);

    // Restore state from before deep sleep, or default to library
    if (wakingFromSleep) {
        Preferences prefs;
        prefs.begin("ereader", true);  // read-only
        int savedState = prefs.getInt("sleepState", (int)STATE_LIBRARY);
        libraryScroll = prefs.getInt("sleepLibScrl", 0);
        String savedBook = prefs.getString("sleepBook", "");
        prefs.end();

        Serial.printf("Wake: restoring state=%d libScrl=%d book=%s\n",
            savedState, libraryScroll, savedBook.c_str());

        // Clamp libraryScroll to valid range
        if (libraryScroll < 0) libraryScroll = 0;
        if (libraryScroll >= (int)books.size()) libraryScroll = 0;

        bool resumedReader = false;
        if (savedState == (int)STATE_READER && savedBook.length() > 0) {
            debug_trace_mark("wake:before_openBook", savedBook);
            // ── Crash guard: increment count BEFORE the risky openBook call ──
            {
                Preferences cgPrefs;
                cgPrefs.begin("ereader", false);
                int crashCount = cgPrefs.getInt("crashCount", 0);
                crashCount++;
                cgPrefs.putInt("crashCount", crashCount);
                cgPrefs.end();

                if (crashCount >= 2) {
                    Serial.println("Crash guard triggered: clearing saved book");
                    Preferences clrPrefs;
                    clrPrefs.begin("ereader", false);
                    clrPrefs.putString("sleepBook", "");
                    clrPrefs.putInt("crashCount", 0);
                    clrPrefs.end();
                    savedBook = "";
                }
            }

            if (savedBook.length() > 0) {
                // Reopen the book — openBook() calls loadProgress() internally,
                // which restores chapter + page from the progress JSON on SD.
                if (reader.openBook(savedBook.c_str())) {
                    debug_trace_mark("wake:after_openBook", savedBook);
                    // Book opened successfully — reset crash guard
                    Preferences okPrefs;
                    okPrefs.begin("ereader", false);
                    okPrefs.putInt("crashCount", 0);
                    okPrefs.end();

                    appState = STATE_READER;
                    readerRefresh.fastRefresh = false;
                    readerRefresh.forceFullRefresh = true;
                    readerRefresh.pageTurnsSinceFull = 0;
                    resumedReader = true;
                    Serial.printf("Wake: resumed reader — %s ch%d pg%d\n",
                        reader.getTitle().c_str(),
                        reader.getCurrentChapter(), reader.getCurrentPage());
                } else {
                    Serial.printf("Wake: failed to reopen %s, falling back to library\n",
                        savedBook.c_str());
                }
            }
        }

        if (!resumedReader) {
            appState = STATE_LIBRARY;
        }
        debug_trace_mark("wake:post_restore", String((int)appState));

        // Draw the restored screen immediately after the wake banner so the
        // user gets instant acknowledgement first, then the restored content.
        firstLibraryDraw = true;
        needsRedraw = true;
        if (appState == STATE_READER) {
            debug_trace_mark("wake:before_draw_reader");
            drawReaderScreen();
            debug_trace_mark("wake:after_draw_reader");
        } else {
            debug_trace_mark("wake:before_draw_library");
            drawLibraryScreen();
            debug_trace_mark("wake:after_draw_library");
        }
        needsRedraw = false;
        Serial.println("Wake: immediate screen draw complete");
    } else {
        appState = STATE_LIBRARY;
        needsRedraw = true;
    }

    lastActivity = millis();
    lastTouchOrButtonTime = millis();
    bootTime = millis();

    // Configure light sleep with GPIO wakeup
    configureLightSleep();

    debug_trace_mark("setup:complete");
    Serial.println("Setup complete");
}

// Helper: advance or go back one page via the physical button
static void buttonPageForward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.nextPage()) {
            readerRefresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
        const Settings& s = settings_get();
        int numVis = (int)filteredIndices.size();
        int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
        if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
        int itemsPerPage;
        if (s.libraryViewMode == 1) {
            int posterH = 310;
            int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
            itemsPerPage = rowsVisible * 2;
        } else {
            itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
        }
        if (libraryScroll + itemsPerPage < numVis) {
            libraryScroll += itemsPerPage;
            needsRedraw = true;
        }
    }
}

static void buttonPageBackward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.prevPage()) {
            readerRefresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
        const Settings& s = settings_get();
        int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
        if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
        int itemsPerPage;
        if (s.libraryViewMode == 1) {
            int posterH = 310;
            int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
            itemsPerPage = rowsVisible * 2;
        } else {
            itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
        }
        if (libraryScroll > 0) {
            libraryScroll -= itemsPerPage;
            if (libraryScroll < 0) libraryScroll = 0;
            needsRedraw = true;
        }
    }
}

void loop() {
    if (appState == STATE_WIFI) {
        wifi_upload_handle();
    }

    // Poll top button (GPIO 21): single=next page, double=prev page, hold=sleep — active LOW
    bool btnPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (btnPressed && !btnWasPressed) {
        // Fresh press
        btnDownTime = millis();
        lastTouchOrButtonTime = millis();
    } else if (btnPressed && btnWasPressed) {
        // Still held — check for long-press sleep trigger
        unsigned long heldMs = millis() - btnDownTime;
        // Only allow sleep after wake cooldown expires
        if (heldMs >= BUTTON_POWER_MS && millis() >= wakeCooldownEnd) {
            Serial.println("Top button long-press — entering deep sleep");
            btnPressCount = 0;  // clear any queued presses
            enterDeepSleep(true);
        }
    } else if (!btnPressed && btnWasPressed) {
        // Released — only count as tap if it wasn't a long-press
        unsigned long heldMs = millis() - btnDownTime;
        if (heldMs >= BUTTON_DEBOUNCE_MS && heldMs < BUTTON_POWER_MS) {
            btnPressCount++;
            lastBtnReleaseTime = millis();
        }
    }
    btnWasPressed = btnPressed;

    // Resolve single vs double press after the window expires
    if (btnPressCount > 0 && !btnPressed &&
        (millis() - lastBtnReleaseTime >= DOUBLE_PRESS_WINDOW_MS)) {
        if (btnPressCount >= 2) {
            Serial.println("Top button double-press — previous page");
            buttonPageBackward();
        } else {
            Serial.println("Top button single-press — next page");
            buttonPageForward();
        }
        btnPressCount = 0;
    }

    // Poll touch with long-press detection
    static bool lastTouchState = false;
    static TouchPoint lastTouchPt;

    TouchPoint currentPt;
    bool currentTouch = touch_read(currentPt);

    if (currentTouch && !lastTouchState) {
        // Touch down
        lastTouchPt = currentPt;
        touchDownTime = millis();
        touchHandled = false;
        lastTouchOrButtonTime = millis();
    } else if (currentTouch && lastTouchState) {
        // Touch held — keep awake
        lastTouchOrButtonTime = millis();
    } else if (!currentTouch && lastTouchState) {
        // Touch up — process tap or swipe
        lastActivity = millis();

        if (!touchHandled) {
            int tx = lastTouchPt.x;
            int ty = lastTouchPt.y;
            int dx = 0;
            int dy = 0;
            int absDx = 0;
            int absDy = 0;
            unsigned long duration = millis() - touchDownTime;
            bool isLongPress = (duration >= LONG_PRESS_MS);

            // Detect horizontal swipe in reader and library modes.
            // We only have a valid end coordinate while the finger is still down;
            // on release currentPt may be stale/undefined, so treat release as tap
            // unless/until we add explicit move tracking.
            bool swipeHandled = false;
            if (absDx > 60 && absDy < 80) {
                if (appState == STATE_READER) {
                    if (dx < 0) {
                        // Swipe left → next page
                        if (reader.nextPage()) {
                            readerRefresh.fastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        // Swipe right → prev page
                        if (reader.prevPage()) {
                            readerRefresh.fastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    }
                } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
                    // Library swipe: compute items per page for current view mode
                    const Settings& s = settings_get();
                    int numVis = (int)filteredIndices.size();
                    int itemsPerPage;
                    int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
                    if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
                    if (s.libraryViewMode == 1) {
                        int posterH = 310;
                        int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
                        itemsPerPage = rowsVisible * 2;
                    } else {
                        itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
                    }
                    if (dx < 0) {
                        if (libraryScroll + itemsPerPage < numVis) {
                            libraryScroll += itemsPerPage;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        if (libraryScroll > 0) {
                            libraryScroll -= itemsPerPage;
                            if (libraryScroll < 0) libraryScroll = 0;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    }
                }
            }

            // Fall back to tap handling if not a swipe
            if (!swipeHandled) {
                switch (appState) {
                    case STATE_LIBRARY:   handleLibraryTouch(tx, ty);           break;
                    case STATE_READER:    handleReaderTouch(tx, ty, isLongPress); break;
                    case STATE_MENU:      handleMenuTouch(tx, ty);              break;
                    case STATE_GOTO:      handleGotoTouch(tx, ty);              break;
                    case STATE_TOC:       handleTocTouch(tx, ty);               break;
                    case STATE_BOOKMARKS: handleBookmarksTouch(tx, ty);         break;
                    case STATE_SETTINGS:  handleSettingsTouch(tx, ty);          break;
                    case STATE_OTA_CHECK: handleOtaTouch(tx, ty);              break;
                    case STATE_WIFI:      handleWifiTouch(tx, ty);              break;
                    case STATE_OPDS_BROWSE:
                    case STATE_OPDS_DOWNLOAD: {
                        opds_store_handle_touch(tx, ty);
                        OpdsStoreState& os = opds_store_state();
                        if (os.statusMsg == "__BACK__") {
                            os.statusMsg = "";
                            if (opds_store_needs_library_refresh()) {
                                books = library_scan();
                                cover_cache_clear();
                                opds_store_clear_refresh_flag();
                            }
                            appState = STATE_LIBRARY;
                        }
                        needsRedraw = true;
                        break;
                    }
                    default: break;
                }
            }
        }
    }
    lastTouchState = currentTouch;

    if (appState == STATE_OTA_CHECK && otaState.phase == OTA_CHECKING && !needsRedraw) {
        ui_ota_tick(otaState);
        needsRedraw = true;
    }

    // Redraw if needed
    if (needsRedraw) {
        switch (appState) {
            case STATE_LIBRARY:   drawLibraryScreen();   break;
            case STATE_READER:    drawReaderScreen();    break;
            case STATE_MENU:      drawMenuOverlay();     break;
            case STATE_GOTO:      drawGotoScreen();      break;
            case STATE_TOC:       drawTocScreen();       break;
            case STATE_BOOKMARKS: drawBookmarksScreen(); break;
            case STATE_SETTINGS:  drawSettingsScreen();  break;
            case STATE_OTA_CHECK: drawOtaScreen();       break;
            case STATE_WIFI:      drawWifiScreen();      break;
            case STATE_OPDS_BROWSE:
            case STATE_OPDS_DOWNLOAD:
                opds_store_draw();
                needsRedraw = false;
                break;
            default: break;
        }
    }

    // Pre-caching removed — lines are stored on SD card now

    // Deep sleep check
    unsigned long sleepMs = (unsigned long)settings_get().sleepTimeoutMin * 60UL * 1000UL;
    if (millis() - lastActivity > sleepMs) {
        enterDeepSleep();
    }

    // Light sleep instead of busy-wait delay when idle
    if (canEnterLightSleep()) {
        esp_light_sleep_start();
        // Woke up from light sleep — immediately continue to poll touch/button
    } else {
        delay(TOUCH_POLL_MS);
    }
}
