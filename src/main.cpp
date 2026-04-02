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
#include <WiFi.h>
#include <Preferences.h>

// ─── App State ──────────────────────────────────────────────────────
enum AppState {
    STATE_BOOT,
    STATE_LIBRARY,
    STATE_READER,
    STATE_WIFI,
    STATE_MENU,         // redesigned reader overlay menu
    STATE_TOC,          // table of contents
    STATE_BOOKMARKS,    // bookmark list
    STATE_SETTINGS,     // settings page
    STATE_OTA_CHECK     // OTA update check / download
};

static AppState       appState = STATE_BOOT;
static bool           firstLibraryDraw = true;  // full refresh after splash to clear ghost
static bool           settingsFromLibrary = false;  // quicker refresh on library→settings transition
static unsigned long  lastActivity = 0;
static bool           needsRedraw = true;
static bool           readerFastRefresh = false;
static bool           readerChapterJump = false;   // use medium refresh after chapter/bookmark jump
static bool           readerForceFullRefresh = false;  // full refresh on wake resume
static int            readerPageTurnsSinceFull = 0;
static const int      READER_REFRESH_OVERDRAW_PX = 16;

// ─── Library state ──────────────────────────────────────────────────
static std::vector<BookInfo> books;
static int libraryScroll = 0;

// ─── Reader state ───────────────────────────────────────────────────
static BookReader reader;

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

// ─── OTA state ──────────────────────────────────────────────────────
enum OtaPhase { OTA_IDLE, OTA_CHECKING, OTA_RESULT, OTA_DOWNLOADING, OTA_DONE, OTA_FAILED };
static OtaPhase   otaPhase = OTA_IDLE;
static String      otaLatestVersion;
static bool        otaUpdateAvailable = false;
static int         otaProgress = 0;

// ─── Layout helpers ─────────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;  // UI font height (medium FiraSans advance_y)

// Touch zones for reader (portrait)
static const int TOUCH_LEFT  = W / 3;
static const int TOUCH_RIGHT = W * 2 / 3;

// Book list item height
static const int BOOK_ITEM_H = FONT_H * 2 + 12;  // title + info + padding

// Menu/list item height
static const int MENU_ITEM_H = FONT_H + 24;

// Settings row height
static const int SETTINGS_ROW_H = FONT_H + 8;   // 58px — fits 12 rows + reset in 818px available

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

static void drawHeader(const char* title, bool showBattery = true) {
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

static void drawBottomBar(const char* label) {
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

static void drawGnomeSplash() {
    display_fill_screen(15);

    for (int y = 0; y < SPLASH_HEIGHT; ++y) {
        int rowOffset = y * ((SPLASH_WIDTH + 7) / 8);
        for (int x = 0; x < SPLASH_WIDTH; ++x) {
            uint8_t byte = pgm_read_byte(&GNOME_SPLASH_BITMAP[rowOffset + (x / 8)]);
            bool black = (byte & (0x80 >> (x & 7))) == 0;
            display_draw_pixel(x, y, black ? 0 : 15);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Library screen
// ═══════════════════════════════════════════════════════════════════

static void wrapPosterTitle(const String& title, int maxWidth, int maxLines,
                            std::vector<String>& outLines) {
    outLines.clear();
    int start = 0;
    int len = title.length();

    while (start < len && (int)outLines.size() < maxLines) {
        while (start < len && title[start] == ' ') start++;
        if (start >= len) break;

        int bestBreak = start;
        int pos = start;
        while (pos <= len) {
            int nextSpace = title.indexOf(' ', pos);
            if (nextSpace < 0) nextSpace = len;
            String candidate = title.substring(start, nextSpace);
            if (display_text_width(candidate.c_str()) <= maxWidth) {
                bestBreak = nextSpace;
                pos = nextSpace + 1;
                if (nextSpace == len) break;
            } else {
                break;
            }
        }

        if (bestBreak == start) {
            int cut = start + 1;
            while (cut <= len && display_text_width(title.substring(start, cut).c_str()) <= maxWidth) cut++;
            bestBreak = max(start + 1, cut - 1);
        }

        String line = title.substring(start, bestBreak);
        line.trim();
        if (line.length() > 0) outLines.push_back(line);
        start = bestBreak + 1;
    }

    if (start < len && !outLines.empty()) {
        String tail = outLines.back();
        while (tail.length() > 3 && display_text_width((tail + "...").c_str()) > maxWidth) {
            tail = tail.substring(0, tail.length() - 1);
        }
        outLines.back() = tail + "...";
    }
}

static void drawDefaultPoster(const BookInfo& book, int x, int y, int w, int h) {
    if (settings_get().posterShowCovers && cover_render_poster(book, x, y, w, h)) {
        return;
    }

    display_draw_filled_rect(x, y, w, h, 15);
    display_draw_rect(x, y, w, h, 8);
    display_draw_filled_rect(x + 10, y + 10, w - 20, 26, 13);

    const char* band = "BOOK";
    int bandW = display_text_width(band);
    display_draw_text(x + (w - bandW) / 2, y + 32, band, 6);

    std::vector<String> titleLines;
    wrapPosterTitle(book.title, w - 30, 3, titleLines);

    int titleBlockTop = y + 70;
    int lineStep = FONT_H - 6;
    int availH = h - 110;  // space between band and bottom info
    int totalHeight = (int)titleLines.size() * lineStep;
    int ty = titleBlockTop + max(0, (availH - totalHeight) / 2);
    for (const auto& line : titleLines) {
        int lw = display_text_width(line.c_str());
        display_draw_text(x + (w - lw) / 2, ty, line.c_str(), 0);
        ty += lineStep;
    }

    // Bottom area: progress on left, status on right — no overlap
    if (book.hasProgress && book.totalChapters > 0) {
        int pct = (book.progressChapter * 100) / max(1, book.totalChapters);
        if (pct > 100) pct = 100;
        char pctStr[10];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        display_draw_text(x + 14, y + h - 18, pctStr, 4);
    }
}

static void drawLibraryScreen() {
    display_set_font_size(1);  // UI always uses medium font
    display_fill_screen(15);
    drawHeader("Library");
    const Settings& s = settings_get();

    if (books.empty()) {
        int cy = H / 2 - 60;
        const char* line1 = "No books found";
        int w1 = display_text_width(line1);
        display_draw_text((W - w1) / 2, cy, line1, 0);

        cy += FONT_H + 10;
        const char* line2 = "Place .epub files in";
        int w2 = display_text_width(line2);
        display_draw_text((W - w2) / 2, cy, line2, 6);

        cy += FONT_H + 4;
        const char* line3 = "/books/ on the SD card";
        int w3 = display_text_width(line3);
        display_draw_text((W - w3) / 2, cy, line3, 6);

        cy += FONT_H + 4;
        const char* line4 = "or use Settings > Upload";
        int w4 = display_text_width(line4);
        display_draw_text((W - w4) / 2, cy, line4, 6);
    } else if (s.libraryViewMode == 1) {
        int y = HEADER_HEIGHT + MARGIN_Y;

        int currentIdx = library_find_current_book(books);
        if (currentIdx >= 0) {
            display_draw_filled_rect(0, y, W, FONT_H + 16, 14);
            display_draw_text(MARGIN_X, y + FONT_H + 2, "Continue Reading:", 4);

            String contTitle = books[currentIdx].title;
            int labelW = display_text_width("Continue Reading: ");
            int maxTitleW = W - MARGIN_X * 2 - labelW;
            while (contTitle.length() > 3 && display_text_width(contTitle.c_str()) > maxTitleW) {
                contTitle = contTitle.substring(0, contTitle.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X + labelW, y + FONT_H + 2, contTitle.c_str(), 0);
            display_draw_hline(0, y + FONT_H + 16, W, 10);
            y += FONT_H + 20;
        }

        const int cols = 2;
        const int gap = 18;
        int posterW = (W - MARGIN_X * 2 - gap) / cols;
        int posterH = 310;
        int rowsVisible = max(1, (H - y - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
        int cardsPerPage = rowsVisible * cols;

        for (int i = libraryScroll; i < (int)books.size() && i < libraryScroll + cardsPerPage; i++) {
            int rel = i - libraryScroll;
            int row = rel / cols;
            int col = rel % cols;
            int px = MARGIN_X + col * (posterW + gap);
            int py = y + row * (posterH + 14);
            drawDefaultPoster(books[i], px, py, posterW, posterH);
        }

        // Paging controls — always show page indicator, show arrows when multi-page
        int pageInfoY = H - FOOTER_HEIGHT - FONT_H - 8;
        if ((int)books.size() > cardsPerPage) {
            bool hasPrev = (libraryScroll > 0);
            bool hasNext = (libraryScroll + cardsPerPage < (int)books.size());

            // Left arrow
            if (hasPrev) {
                display_draw_text(MARGIN_X, pageInfoY, "< Prev", 3);
            }

            // Page info center
            char info[32];
            int curPage = libraryScroll / max(1, cardsPerPage) + 1;
            int totalPg = ((int)books.size() + cardsPerPage - 1) / cardsPerPage;
            snprintf(info, sizeof(info), "%d / %d", curPage, totalPg);
            int iw = display_text_width(info);
            display_draw_text(W / 2 - iw / 2, pageInfoY, info, 6);

            // Right arrow
            if (hasNext) {
                const char* next = "Next >";
                int nw = display_text_width(next);
                display_draw_text(W - MARGIN_X - nw, pageInfoY, next, 3);
            }
        } else if ((int)books.size() > 0) {
            char info[32];
            snprintf(info, sizeof(info), "%d books", (int)books.size());
            int iw = display_text_width(info);
            display_draw_text(W / 2 - iw / 2, pageInfoY, info, 8);
        }
    } else {
        int y = HEADER_HEIGHT + MARGIN_Y;

        // "Continue Reading" banner
        int currentIdx = library_find_current_book(books);
        if (currentIdx >= 0) {
            display_draw_filled_rect(0, y, W, FONT_H + 16, 14);
            display_draw_text(MARGIN_X, y + FONT_H + 2, "Continue Reading:", 4);

            String contTitle = books[currentIdx].title;
            int labelW = display_text_width("Continue Reading: ");
            int maxTitleW = W - MARGIN_X * 2 - labelW;
            while (contTitle.length() > 3 &&
                   display_text_width(contTitle.c_str()) > maxTitleW) {
                contTitle = contTitle.substring(0, contTitle.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X + labelW, y + FONT_H + 2, contTitle.c_str(), 0);

            display_draw_hline(0, y + FONT_H + 16, W, 10);
            y += FONT_H + 20;
        }

        int maxVisible = (H - y - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;

        for (int i = libraryScroll;
             i < (int)books.size() && i < libraryScroll + maxVisible; i++) {
            String title = books[i].title;

            int badgeW = 0;
            char badge[16] = "";
            if (books[i].hasProgress && books[i].totalChapters > 0) {
                int pct = (books[i].progressChapter * 100) / books[i].totalChapters;
                if (pct > 100) pct = 100;
                snprintf(badge, sizeof(badge), "%d%%", pct);
                badgeW = display_text_width(badge) + 10;
            }

            int titleMaxW = W - MARGIN_X * 2 - badgeW;
            while (title.length() > 3 &&
                   display_text_width(title.c_str()) > titleMaxW) {
                title = title.substring(0, title.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X, y + FONT_H - 4, title.c_str(), 0);

            if (badge[0]) {
                int bw = display_text_width(badge);
                display_draw_text(W - MARGIN_X - bw, y + FONT_H - 4, badge, 6);
            }

            String info = books[i].filepath;
            int ls = info.lastIndexOf('/');
            if (ls >= 0) info = info.substring(ls + 1);
            if (info.length() > 40) info = info.substring(0, 37) + "...";
            display_draw_text(MARGIN_X, y + FONT_H * 2 - 8, info.c_str(), 8);

            display_draw_hline(MARGIN_X, y + BOOK_ITEM_H - 2,
                               W - MARGIN_X * 2, 12);
            y += BOOK_ITEM_H;
        }

        // Paging controls for list view
        int pageInfoY = H - FOOTER_HEIGHT - FONT_H - 8;
        if ((int)books.size() > maxVisible) {
            bool hasPrev = (libraryScroll > 0);
            bool hasNext = (libraryScroll + maxVisible < (int)books.size());

            if (hasPrev) {
                display_draw_text(MARGIN_X, pageInfoY, "< Prev", 3);
            }

            char info[32];
            int curPage = libraryScroll / max(1, maxVisible) + 1;
            int totalPg = ((int)books.size() + maxVisible - 1) / maxVisible;
            snprintf(info, sizeof(info), "%d / %d", curPage, totalPg);
            int iw = display_text_width(info);
            display_draw_text(W / 2 - iw / 2, pageInfoY, info, 6);

            if (hasNext) {
                const char* next = "Next >";
                int nw = display_text_width(next);
                display_draw_text(W - MARGIN_X - nw, pageInfoY, next, 3);
            }
        }
    }

    drawBottomBar("[ Settings ]");

    if (firstLibraryDraw) {
        firstLibraryDraw = false;
        display_update();  // full refresh after splash — clears e-ink ghost
    } else {
        display_update_medium();  // 2-cycle refresh — quieter than fast (less boost converter switching)
    }
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Reader screen
// ═══════════════════════════════════════════════════════════════════

static void drawReaderScreen() {
    const Settings& s = settings_get();
    display_set_font_size(s.fontSize);  // use reader's selected font
    int marginX;
    int lineSpacing;
    switch (s.fontSize) {
        case 0: lineSpacing = FONT_LINE_SPACING_SMALL;  marginX = FONT_MARGIN_X_SMALL;  break;
        case 2: lineSpacing = FONT_LINE_SPACING_LARGE;  marginX = FONT_MARGIN_X_LARGE;  break;
        default: lineSpacing = FONT_LINE_SPACING_MEDIUM; marginX = FONT_MARGIN_X_MEDIUM; break;
    }

    display_fill_screen(15);

    // Minimal header — anchored to HEADER_HEIGHT
    {
        int headerBaseline = HEADER_HEIGHT - 18;

        String title = reader.getTitle();
        // Truncate title by pixel width to fit with battery and bookmark indicator
        String headerStr;
        if (reader.isCurrentPageBookmarked()) {
            headerStr = "* ";
        }

        int maxTitleW = W - marginX * 2;
        if (s.showBattery) maxTitleW -= 80;  // reserve space for battery
        if (headerStr.length() > 0) maxTitleW -= display_text_width(headerStr.c_str());

        while (title.length() > 3 &&
               display_text_width(title.c_str()) > maxTitleW) {
            title = title.substring(0, title.length() - 4) + "...";
        }
        headerStr += title;

        display_draw_text(marginX, headerBaseline, headerStr.c_str(), 10);

        if (s.showBattery) {
            char battStr[8];
            snprintf(battStr, sizeof(battStr), "%d%%", battery_percent());
            int bw = display_text_width(battStr);
            display_draw_text(W - marginX - bw, headerBaseline, battStr, 10);
        }

        display_draw_hline(marginX, HEADER_HEIGHT - 4, W - marginX * 2, 13);
    }

    // Page text — body top must be below header divider + font ascender.
    // Use the active reader font metrics here, not the fixed UI font height,
    // or pagination and on-screen drawing drift apart and body text can spill
    // into the footer/progress bar.
    const auto& lines = reader.getPageLines();
    int bodyTop = HEADER_HEIGHT + MARGIN_Y;  // top edge of body area
    int fontAscender = display_font_ascender();
    int bodyFontH = display_font_height();
    int lineH = bodyFontH + lineSpacing;
    int y = bodyTop + fontAscender;  // first baseline
    int bodyBottom = H - FOOTER_HEIGHT - 2;  // minimal guard above the footer area

    if (lines.empty()) {
        display_draw_text(marginX, y, "[Page content unavailable]", 6);
    } else {
        for (const auto& line : lines) {
            if (y > bodyBottom) break;

            if (inline_image_is_marker(line)) {
                // Render inline image
                String imgPath; int imgW, imgH, imgLines;
                if (inline_image_parse_enriched(line, imgPath, imgW, imgH, imgLines)) {
                    // Center image horizontally in body area
                    int imgX = marginX + (reader.getTotalPages() > 0 ?
                               (W - marginX * 2 - imgW) / 2 : 0);
                    // Image top = current line top (baseline - ascender)
                    int imgY = y - fontAscender;
                    inline_image_render(reader.getParser(), imgPath, imgX, imgY, imgW, imgH);
                    // Advance Y by the image height (continuation lines handle the rest)
                }
                y += lineH;
            } else if (inline_image_is_continuation(line)) {
                // Image continuation placeholder — just advance Y
                y += lineH;
            } else {
                display_draw_text(marginX, y, line.c_str(), 0);
                y += lineH;
            }
        }
    }

    // Progress bar
    int barY = H - FOOTER_HEIGHT;
    int totalPages = reader.getTotalPages();
    int curPage = reader.getCurrentPage();
    float progress = (totalPages > 1) ? (float)curPage / (totalPages - 1) : 1.0f;
    int barW = W - marginX * 2;
    int filledW = (int)(barW * progress);

    display_draw_filled_rect(marginX, barY, barW, PROGRESS_BAR_H, 12);
    if (filledW > 0)
        display_draw_filled_rect(marginX, barY, filledW, PROGRESS_BAR_H, 0);

    // Footer text
    if (s.showPageNumbers) {
        int footY = barY + PROGRESS_BAR_H + FONT_H - 6;
        char chInfo[32];
        snprintf(chInfo, sizeof(chInfo), "Ch %d/%d",
                 reader.getCurrentChapter() + 1, reader.getTotalChapters());
        display_draw_text(marginX, footY, chInfo, 8);

        int pct = 0;
        if (reader.getTotalChapters() > 0)
            pct = (reader.getCurrentChapter() * 100) / reader.getTotalChapters();
        char pctStr[8];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        int pctW = display_text_width(pctStr);
        display_draw_text((W - pctW) / 2, footY, pctStr, 6);

        char pgInfo[32];
        snprintf(pgInfo, sizeof(pgInfo), "%d / %d", curPage + 1, totalPages);
        int pw = display_text_width(pgInfo);
        display_draw_text(W - marginX - pw, footY, pgInfo, 8);
    }

    // Reader uses a CrossPoint-style hybrid: fast localized cleanup for the
    // reading body on ordinary page turns, with periodic stronger body-only
    // cleanup to keep ghosting from accumulating.
    if (readerForceFullRefresh) {
        // Full refresh to cleanly replace sleep image on wake resume
        display_update();
        readerForceFullRefresh = false;
        readerPageTurnsSinceFull = 0;
    } else if (readerFastRefresh) {
        int refreshInterval = settings_get().refreshEveryPages;
        if (refreshInterval < 1) refreshInterval = 4;
        bool strongCleanup = (readerPageTurnsSinceFull + 1 >= refreshInterval);

        if (strongCleanup) {
            display_update_medium();
            readerPageTurnsSinceFull = 0;
        } else {
            display_update_fast();
            readerPageTurnsSinceFull++;
        }
    } else if (readerChapterJump) {
        display_update_fast();
        readerPageTurnsSinceFull = 0;
        readerChapterJump = false;
    } else {
        display_update_fast();
        readerPageTurnsSinceFull = 0;
    }
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Menu overlay (redesigned)
// ═══════════════════════════════════════════════════════════════════

// Menu item Y positions for touch detection
static const int MENU_START_Y = 225;  // dropped ~5mm to avoid overlapping header
static const int LIST_NAV_Y = H - FOOTER_HEIGHT - FONT_H - 8;

static void drawMenuOverlay() {
    display_fill_screen(15);

    // Title — truncate by pixel width to fit screen
    String title = reader.getTitle();
    int maxMenuTitleW = W - MARGIN_X * 2;
    while (title.length() > 3 &&
           display_text_width(title.c_str()) > maxMenuTitleW) {
        title = title.substring(0, title.length() - 4) + "...";
    }
    int tw = display_text_width(title.c_str());
    display_draw_text((W - tw) / 2, 80, title.c_str(), 0);

    // Progress info — use compact format to fit 540px width
    char progStr[64];
    snprintf(progStr, sizeof(progStr), "Ch %d/%d  Pg %d/%d",
             reader.getCurrentChapter() + 1, reader.getTotalChapters(),
             reader.getCurrentPage() + 1, reader.getTotalPages());
    int pw = display_text_width(progStr);
    display_draw_text((W - pw) / 2, 130, progStr, 6);

    // Reading statistics line
    reader.updateReadingTime();
    uint32_t totalSec = reader.getTotalReadingTimeSec();
    uint32_t totalPages = reader.getTotalPagesRead();
    if (totalSec > 0 || totalPages > 0) {
        char statsStr[64];
        if (totalSec >= 3600) {
            snprintf(statsStr, sizeof(statsStr), "%dh %dm read  %d pages",
                     (int)(totalSec / 3600), (int)((totalSec % 3600) / 60), (int)totalPages);
        } else {
            snprintf(statsStr, sizeof(statsStr), "%dm read  %d pages",
                     (int)(totalSec / 60), (int)totalPages);
        }
        int sw = display_text_width(statsStr);
        display_draw_text((W - sw) / 2, 165, statsStr, 8);
    }

    display_draw_hline(MARGIN_X, 185, W - MARGIN_X * 2, 10);

    // Menu items
    int y = MENU_START_Y;
    const int indent = MARGIN_X + 30;

    display_draw_text(indent, y, "Table of Contents", 0);
    display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
    y += MENU_ITEM_H;

    char bmLabel[32];
    snprintf(bmLabel, sizeof(bmLabel), "Bookmarks (%d)", reader.getBookmarks().size());
    display_draw_text(indent, y, bmLabel, 0);
    display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
    y += MENU_ITEM_H;

    display_draw_text(indent, y, "Settings", 0);
    display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
    y += MENU_ITEM_H;

    display_draw_text(indent, y, "Library", 0);
    display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
    y += MENU_ITEM_H;

    // Hint at bottom
    const char* hint = "Tap outside to resume";
    int hw = display_text_width(hint);
    if (reader.isCurrentPageBookmarked()) {
        const char* mark = "This page is bookmarked";
        int mw = display_text_width(mark);
        display_draw_text((W - mw) / 2, H - 132, mark, 6);
    }
    display_draw_text((W - hw) / 2, H - 100, hint, 10);

    display_update_fast();
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// TOC screen
// ═══════════════════════════════════════════════════════════════════

static void drawTocScreen() {
    display_fill_screen(15);
    drawHeader("Table of Contents");

    int y = HEADER_HEIGHT + MARGIN_Y;
    int totalEntries = reader.getTocCount();
    int maxVisible = (H - y - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;
    int currentCh = reader.getCurrentChapter();

    for (int i = tocScroll; i < totalEntries && i < tocScroll + maxVisible; i++) {
        int tocChapter = reader.getTocChapterIndex(i);
        // Highlight current chapter
        if (tocChapter == currentCh) {
            display_draw_filled_rect(0, y - 8, W, MENU_ITEM_H, 14);
            display_draw_text(MARGIN_X, y + FONT_H - 8, ">", 0);
        }

        String chTitle = reader.getTocLabel(i);
        chTitle.trim();
        if (chTitle.length() == 0 || chTitle.equalsIgnoreCase("unknown")) {
            if (tocChapter >= 0) {
                chTitle = reader.getChapterTitle(tocChapter);
                chTitle.trim();
            }
        }
        if (chTitle.length() == 0 || chTitle.equalsIgnoreCase("unknown")) {
            if (tocChapter >= 0) {
                chTitle = String("Chapter ") + String(tocChapter + 1);
            } else {
                chTitle = String("Section ") + String(i + 1);
            }
        }

        char prefix[20];
        if (tocChapter >= 0) {
            snprintf(prefix, sizeof(prefix), "%d. ", tocChapter + 1);
        } else {
            snprintf(prefix, sizeof(prefix), "%d. ", i + 1);
        }
        int prefixW = display_text_width(prefix);
        int maxTitleW = W - MARGIN_X * 2 - 30 - prefixW;
        // Truncate by pixel width
        while (chTitle.length() > 3 &&
               display_text_width(chTitle.c_str()) > maxTitleW) {
            chTitle = chTitle.substring(0, chTitle.length() - 4) + "...";
        }

        char label[160];
        snprintf(label, sizeof(label), "%s%s", prefix, chTitle.c_str());
        display_draw_text(MARGIN_X + 30, y + FONT_H - 8, label, (tocChapter == currentCh) ? 0 : 3);

        display_draw_hline(MARGIN_X, y + MENU_ITEM_H - 4, W - MARGIN_X * 2, 13);
        y += MENU_ITEM_H;
    }

    // Scroll indicator
    if (totalEntries > maxVisible) {
        char info[32];
        snprintf(info, sizeof(info), "%d-%d of %d",
                 tocScroll + 1,
                 min(tocScroll + maxVisible, totalEntries),
                 totalEntries);
        int iw = display_text_width(info);
        if (tocScroll > 0) {
            display_draw_text(MARGIN_X, LIST_NAV_Y, "< Prev", 3);
        }
        display_draw_text(W / 2 - iw / 2, LIST_NAV_Y, info, 8);
        if (tocScroll + maxVisible < totalEntries) {
            const char* next = "Next >";
            int nw = display_text_width(next);
            display_draw_text(W - MARGIN_X - nw, LIST_NAV_Y, next, 3);
        }
    }

    drawBottomBar("[ Back to Reading ]");
    display_update();  // full refresh — clears ghost bleed
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Bookmarks screen
// ═══════════════════════════════════════════════════════════════════

static void drawBookmarksScreen() {
    display_fill_screen(15);
    drawHeader("Bookmarks");

    const auto& bmarks = reader.getBookmarks();
    int y = HEADER_HEIGHT + MARGIN_Y;

    if (bmarks.empty()) {
        int cy = H / 2 - 30;
        const char* msg = "No bookmarks yet";
        int mw = display_text_width(msg);
        display_draw_text((W - mw) / 2, cy, msg, 6);

        const char* hint = "Long-press center to add";
        int hw = display_text_width(hint);
        display_draw_text((W - hw) / 2, cy + FONT_H + 8, hint, 8);
    } else {
        int maxVisible = (H - y - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;

        for (int i = bmScroll; i < (int)bmarks.size() && i < bmScroll + maxVisible; i++) {
            String label = String(i + 1) + ". " + bmarks[i].label;
            while (label.length() > 6 && display_text_width(label.c_str()) > W - MARGIN_X * 2 - 40) {
                label = label.substring(0, label.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X, y + FONT_H - 8, label.c_str(), 0);

            // Show "x" on right to hint at delete (tap right side)
            display_draw_text(W - MARGIN_X - 20, y + FONT_H - 8, "x", 8);

            display_draw_hline(MARGIN_X, y + MENU_ITEM_H - 4, W - MARGIN_X * 2, 13);
            y += MENU_ITEM_H;
        }

        if ((int)bmarks.size() > maxVisible) {
            char info[32];
            snprintf(info, sizeof(info), "%d-%d of %d",
                     bmScroll + 1,
                     min(bmScroll + maxVisible, (int)bmarks.size()),
                     (int)bmarks.size());
            int iw = display_text_width(info);
            if (bmScroll > 0) {
                display_draw_text(MARGIN_X, LIST_NAV_Y, "< Prev", 3);
            }
            display_draw_text(W / 2 - iw / 2, LIST_NAV_Y, info, 8);
            if (bmScroll + maxVisible < (int)bmarks.size()) {
                const char* next = "Next >";
                int nw = display_text_width(next);
                display_draw_text(W - MARGIN_X - nw, LIST_NAV_Y, next, 3);
            }
        }
    }

    drawBottomBar("[ Back to Reading ]");
    display_update();  // full refresh — clears ghost bleed
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Settings screen
// ═══════════════════════════════════════════════════════════════════

static const char* fontSizeNames[] = {"Small", "Medium", "Large"};
static const char* libraryViewNames[] = {"List", "Poster"};
static const int sleepOptions[] = {2, 5, 10, 15, 30};
static const int NUM_SLEEP_OPTIONS = 5;
static const int refreshOptions[] = {1, 2, 4, 6, 10};
static const int NUM_REFRESH_OPTIONS = 5;

static int findSleepIdx() {
    int val = settings_get().sleepTimeoutMin;
    for (int i = 0; i < NUM_SLEEP_OPTIONS; i++) {
        if (sleepOptions[i] == val) return i;
    }
    return 1; // default to 5min
}

static int findRefreshIdx() {
    int val = settings_get().refreshEveryPages;
    for (int i = 0; i < NUM_REFRESH_OPTIONS; i++) {
        if (refreshOptions[i] == val) return i;
    }
    return 1; // default to 4 pages
}

static void drawSettingsScreen() {
    display_fill_screen(15);
    drawHeader("Settings");

    Settings& s = settings_get();
    int y = HEADER_HEIGHT + MARGIN_Y + 10;
    const int labelX = MARGIN_X;
    const int valueX = W - MARGIN_X - 160;

    // Font Size — draw label in UI font, then preview in selected font
    display_draw_text(labelX, y + FONT_H - 4, "Font Size", 0);
    char fsLabel[32];
    snprintf(fsLabel, sizeof(fsLabel), "< %s >", fontSizeNames[s.fontSize]);
    int fsw = display_text_width(fsLabel);
    display_draw_text(W - MARGIN_X - fsw, y + FONT_H - 4, fsLabel, 3);
    display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
    y += SETTINGS_ROW_H;

    // Font preview line in the selected font size
    display_set_font_size(s.fontSize);
    const char* preview = "The quick brown fox jumps over the lazy dog";
    display_draw_text(labelX, y + display_font_height() - 4, preview, 6);
    display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
    y += SETTINGS_ROW_H;
    display_set_font_size(1);  // back to UI font

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

    // Library view mode
    display_draw_text(labelX, y + FONT_H - 4, "Library View", 0);
    char lvLabel[32];
    snprintf(lvLabel, sizeof(lvLabel), "< %s >", libraryViewNames[s.libraryViewMode]);
    int lvw = display_text_width(lvLabel);
    display_draw_text(W - MARGIN_X - lvw, y + FONT_H - 4, lvLabel, 3);
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

    // Page Numbers
    display_draw_text(labelX, y + FONT_H - 4, "Page Numbers", 0);
    const char* pnLabel = s.showPageNumbers ? "[ ON ]" : "[ OFF ]";
    int pnw = display_text_width(pnLabel);
    display_draw_text(W - MARGIN_X - pnw, y + FONT_H - 4, pnLabel, 3);
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

    // Reset Defaults button
    y += 20;
    const char* resetLabel = "[ Reset Defaults ]";
    int rw = display_text_width(resetLabel);
    display_draw_text(MARGIN_X, y + FONT_H - 4, resetLabel, 3);

    // Firmware version
    char verLabel[32];
    snprintf(verLabel, sizeof(verLabel), "Firmware: v%s", FIRMWARE_VERSION);
    int vw = display_text_width(verLabel);
    display_draw_text(W - MARGIN_X - vw, y + FONT_H - 4, verLabel, 8);

    drawBottomBar("[ Back ]");
    if (settingsFromLibrary) {
        display_update_medium();  // quicker transition from library
        settingsFromLibrary = false;
    } else {
        display_update();  // full refresh — clears ghost bleed from previous screen
    }
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// OTA Update screen
// ═══════════════════════════════════════════════════════════════════

static void drawOtaScreen() {
    display_fill_screen(15);
    drawHeader("Firmware Update");

    int cy = H / 2 - 40;  // vertical center area

    switch (otaPhase) {
        case OTA_CHECKING: {
            const char* msg = "Checking for updates...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);
            drawBottomBar("[ Cancel ]");
            display_update_fast();
            needsRedraw = false;

            // Perform the check now (blocking but quick)
            if (WiFi.status() != WL_CONNECTED) {
                // Need to connect WiFi first
                Settings& s = settings_get();
                WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());
                unsigned long start = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                    delay(100);
                }
            }

            if (WiFi.status() != WL_CONNECTED) {
                otaPhase = OTA_FAILED;
                otaLatestVersion = "Connect to WiFi first";
                needsRedraw = true;
                return;
            }

            otaUpdateAvailable = ota_check_for_update(otaLatestVersion);
            otaPhase = OTA_RESULT;
            needsRedraw = true;
            return;
        }

        case OTA_RESULT: {
            if (otaUpdateAvailable) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s available", otaLatestVersion.c_str());
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
            needsRedraw = false;
            break;
        }

        case OTA_DOWNLOADING: {
            const char* msg = "Downloading update...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy - 20, msg, 0);

            // Progress bar
            int barX = MARGIN_X + 20;
            int barW = W - barX * 2;
            int barY = cy + 30;
            int barH = 16;
            display_draw_rect(barX, barY, barW, barH, 0);
            int fillW = (barW - 4) * otaProgress / 100;
            if (fillW > 0) {
                display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
            }

            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%d%%", otaProgress);
            int pw = display_text_width(pctStr);
            display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

            display_update_fast();
            needsRedraw = false;
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
            needsRedraw = false;

            delay(2000);
            esp_restart();
            break;
        }

        case OTA_FAILED: {
            const char* msg = "Update failed";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);

            if (otaLatestVersion.length() > 0) {
                int lw = display_text_width(otaLatestVersion.c_str());
                display_draw_text((W - lw) / 2, cy + FONT_H + 16, otaLatestVersion.c_str(), 6);
            }

            drawBottomBar("[ Back ]");
            display_update_fast();
            needsRedraw = false;
            break;
        }

        default:
            break;
    }
}

static void handleOtaTouch(int x, int y) {
    (void)x;

    // Footer → back (cancel)
    if (y > H - FOOTER_HEIGHT) {
        otaPhase = OTA_IDLE;
        appState = STATE_SETTINGS;
        needsRedraw = true;
        return;
    }

    // In result state with update available → start download
    if (otaPhase == OTA_RESULT && otaUpdateAvailable) {
        otaPhase = OTA_DOWNLOADING;
        otaProgress = 0;
        needsRedraw = true;

        // Draw the initial download screen
        drawOtaScreen();

        // Perform the download (blocking with progress callbacks)
        bool ok = ota_install_update([](int pct) {
            otaProgress = pct;
            // Redraw progress every 5%
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
            otaPhase = OTA_DONE;
        } else {
            otaPhase = OTA_FAILED;
            otaLatestVersion = "Try again later";
        }
        needsRedraw = true;
        return;
    }

    // In failed state → back
    if (otaPhase == OTA_FAILED || (otaPhase == OTA_RESULT && !otaUpdateAvailable)) {
        otaPhase = OTA_IDLE;
        appState = STATE_SETTINGS;
        needsRedraw = true;
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════
// WiFi screen
// ═══════════════════════════════════════════════════════════════════

static void drawWifiScreen() {
    display_fill_screen(15);
    drawHeader("WiFi Upload");

    int y = HEADER_HEIGHT + 60;

    if (wifi_upload_running()) {
        const char* l1 = "WiFi connected";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 0);
        y += FONT_H + 20;

        String ipStr = "http://" + wifi_upload_ip();
        int w2 = display_text_width(ipStr.c_str());
        display_draw_text((W - w2) / 2, y, ipStr.c_str(), 0);
        y += FONT_H + 20;

        const char* l3 = "Open in browser to";
        int w3 = display_text_width(l3);
        display_draw_text((W - w3) / 2, y, l3, 6);
        y += FONT_H + 4;

        const char* l4 = "upload .epub files";
        int w4 = display_text_width(l4);
        display_draw_text((W - w4) / 2, y, l4, 6);
    } else {
        const char* l1 = "Connecting to WiFi...";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 4);
    }

    const char* hint = "Tap anywhere to return";
    int hw = display_text_width(hint);
    display_draw_text((W - hw) / 2, H - 100, hint, 10);

    display_update_fast();
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Touch handlers
// ═══════════════════════════════════════════════════════════════════

static void handleLibraryTouch(int x, int y) {
    // Bottom bar — settings only
    if (y > H - FOOTER_HEIGHT) {
        appState = STATE_SETTINGS;
        settingsFromLibrary = true;
        needsRedraw = true;
        return;
    }

    if (books.empty()) return;

    const Settings& s = settings_get();

    // "Continue Reading" banner
    int currentIdx = library_find_current_book(books);
    int listStartY = HEADER_HEIGHT + MARGIN_Y;
    if (currentIdx >= 0) {
        int bannerBottom = listStartY + FONT_H + 20;
        if (y > listStartY && y < bannerBottom) {
            Serial.printf("Continue reading: %s\n", books[currentIdx].filepath.c_str());
            if (reader.openBook(books[currentIdx].filepath.c_str())) {
                readerFastRefresh = false;
                readerPageTurnsSinceFull = 0;
                appState = STATE_READER;
                needsRedraw = true;
            }
            return;
        }
        listStartY = bannerBottom;
    }

    if (s.libraryViewMode == 1) {
        const int cols = 2;
        const int gap = 18;
        int posterW = (W - MARGIN_X * 2 - gap) / cols;
        int posterH = 310;
        int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
        int cardsPerPage = rowsVisible * cols;

        if (y > listStartY) {
            int relY = y - listStartY;
            int row = relY / (posterH + 14);
            int rowOffset = relY % (posterH + 14);
            if (row < rowsVisible && rowOffset < posterH) {
                for (int col = 0; col < cols; col++) {
                    int px = MARGIN_X + col * (posterW + gap);
                    if (x >= px && x < px + posterW) {
                        int idx = libraryScroll + row * cols + col;
                        if (idx >= 0 && idx < (int)books.size() && idx < libraryScroll + cardsPerPage) {
                            Serial.printf("Opening book: %s\n", books[idx].filepath.c_str());
                            if (reader.openBook(books[idx].filepath.c_str())) {
                                readerFastRefresh = false;
                                readerPageTurnsSinceFull = 0;
                                appState = STATE_READER;
                                needsRedraw = true;
                            }
                            return;
                        }
                    }
                }
            }
        }

        // Paging: tap in the page-controls area (below posters, above footer)
        int pageControlY = H - FOOTER_HEIGHT - FONT_H - 30;
        if ((int)books.size() > cardsPerPage && y >= pageControlY && y <= H - FOOTER_HEIGHT) {
            if (x < W / 3 && libraryScroll > 0) {
                // Prev page
                libraryScroll -= cardsPerPage;
                if (libraryScroll < 0) libraryScroll = 0;
                needsRedraw = true;
            } else if (x > W * 2 / 3 && libraryScroll + cardsPerPage < (int)books.size()) {
                // Next page
                libraryScroll += cardsPerPage;
                if (libraryScroll >= (int)books.size()) libraryScroll = max(0, (int)books.size() - cardsPerPage);
                needsRedraw = true;
            }
        }
        return;
    }

    // Book selection (list view)
    int maxVisible = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;

    // Paging controls area (below book list, above footer)
    int pageControlY = H - FOOTER_HEIGHT - FONT_H - 30;
    if ((int)books.size() > maxVisible && y >= pageControlY && y <= H - FOOTER_HEIGHT) {
        if (x < W / 3 && libraryScroll > 0) {
            libraryScroll -= maxVisible;
            if (libraryScroll < 0) libraryScroll = 0;
            needsRedraw = true;
        } else if (x > W * 2 / 3 && libraryScroll + maxVisible < (int)books.size()) {
            libraryScroll += maxVisible;
            if (libraryScroll >= (int)books.size()) libraryScroll = max(0, (int)books.size() - maxVisible);
            needsRedraw = true;
        }
        return;
    }

    if (y > listStartY) {
        int idx = libraryScroll + (y - listStartY) / BOOK_ITEM_H;

        if (idx >= 0 && idx < (int)books.size() && idx < libraryScroll + maxVisible) {
            Serial.printf("Opening book: %s\n", books[idx].filepath.c_str());
            if (reader.openBook(books[idx].filepath.c_str())) {
                readerFastRefresh = false;
                readerPageTurnsSinceFull = 0;
                appState = STATE_READER;
                needsRedraw = true;
            }
        }
    }
}

static void handleReaderTouch(int x, int y, bool isLongPress) {
    // Long-press center zone → add bookmark
    if (isLongPress && x >= TOUCH_LEFT && x <= TOUCH_RIGHT) {
        reader.addBookmark();
        readerFastRefresh = false;
        needsRedraw = true;
        return;
    }

    // Left zone → prev page
    if (x < TOUCH_LEFT) {
        if (reader.prevPage()) {
            // Use medium refresh for chapter transitions (heavier processing)
            readerFastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerChapterJump = true;
            needsRedraw = true;
        }
        return;
    }

    // Right zone → next page
    if (x > TOUCH_RIGHT) {
        if (reader.nextPage()) {
            readerFastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerChapterJump = true;
            needsRedraw = true;
        }
        return;
    }

    // Center short tap → show menu
    readerFastRefresh = false;
    appState = STATE_MENU;
    needsRedraw = true;
}

static void handleMenuTouch(int x, int y) {
    (void)x;
    // Text is drawn at baseline Y — visible glyphs extend ~FONT_H above baseline.
    // Shift touch zone up so it aligns with what the user sees.
    int zoneTop = MENU_START_Y - FONT_H;
    int row = (y - zoneTop) / MENU_ITEM_H;

    if (y >= zoneTop && y < zoneTop + MENU_ITEM_H * 4) {
        switch (row) {
            case 0: // Table of Contents
                readerFastRefresh = false;
                tocScroll = 0;
                appState = STATE_TOC;
                break;
            case 1: // Bookmarks
                readerFastRefresh = false;
                bmScroll = 0;
                appState = STATE_BOOKMARKS;
                break;
            case 2: // Settings
                readerFastRefresh = false;
                appState = STATE_SETTINGS;
                break;
            case 3: // Library
                readerFastRefresh = false;
                reader.saveProgress();
                reader.closeBook();
                appState = STATE_LIBRARY;
                break;
        }
        needsRedraw = true;
        return;
    }

    // Tap outside menu items → back to reader
    readerFastRefresh = false;
    appState = STATE_READER;
    needsRedraw = true;
}

static void handleTocTouch(int x, int y) {
    // Footer → back to reader
    if (y > H - FOOTER_HEIGHT) {
        readerFastRefresh = false;
        appState = STATE_READER;
        needsRedraw = true;
        return;
    }

    int listY = HEADER_HEIGHT + MARGIN_Y;
    int totalEntries = reader.getTocCount();
    int maxVisible = (H - listY - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;

    if (totalEntries > maxVisible && y >= LIST_NAV_Y - 8 && y < H - FOOTER_HEIGHT) {
        if (x < W / 3 && tocScroll > 0) {
            tocScroll = max(0, tocScroll - maxVisible);
            needsRedraw = true;
            return;
        }
        if (x > (W * 2) / 3 && tocScroll + maxVisible < totalEntries) {
            tocScroll = min(max(0, totalEntries - maxVisible), tocScroll + maxVisible);
            needsRedraw = true;
            return;
        }
    }

    if (y > listY) {
        int idx = tocScroll + (y - listY) / MENU_ITEM_H;
        if (idx >= 0 && idx < totalEntries) {
            int chapterIndex = reader.getTocChapterIndex(idx);
            if (chapterIndex < 0) return;

            // Show a "Loading..." screen immediately so the user knows the device
            // is working. jumpToChapter can take several seconds on large chapters
            // (ZIP decompression + HTML strip + text wrap). Without this, the TOC
            // screen stays visible while the CPU works, then the EPD goes blank for
            // the 6-cycle clear, making it look like a freeze/wedge.
            display_fill_screen(15);
            const char* loadMsg = "Loading chapter...";
            int loadW = display_text_width(loadMsg);
            display_draw_text((W - loadW) / 2, H / 2, loadMsg, 6);
            display_update_fast();

            readerFastRefresh = false;
            readerChapterJump = true;
            readerPageTurnsSinceFull = 0;
            reader.jumpToChapter(chapterIndex);

            // Ensure the reader has valid content before switching state.
            // loadChapter() now guarantees non-empty page lines even on failure,
            // so drawReaderScreen() will always render something meaningful
            // instead of leaving the white "Loading..." framebuffer visible.
            appState = STATE_READER;
            needsRedraw = true;
            return;
        }
    }
}

static void handleBookmarksTouch(int x, int y) {
    // Footer → back
    if (y > H - FOOTER_HEIGHT) {
        readerFastRefresh = false;
        appState = STATE_READER;
        needsRedraw = true;
        return;
    }

    const auto& bmarks = reader.getBookmarks();
    if (bmarks.empty()) return;

    int listY = HEADER_HEIGHT + MARGIN_Y;
    int maxVisible = (H - listY - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;

    if ((int)bmarks.size() > maxVisible && y >= LIST_NAV_Y - 8 && y < H - FOOTER_HEIGHT) {
        if (x < W / 3 && bmScroll > 0) {
            bmScroll = max(0, bmScroll - maxVisible);
            needsRedraw = true;
            return;
        }
        if (x > (W * 2) / 3 && bmScroll + maxVisible < (int)bmarks.size()) {
            bmScroll = min(max(0, (int)bmarks.size() - maxVisible), bmScroll + maxVisible);
            needsRedraw = true;
            return;
        }
    }

    if (y > listY) {
        int idx = bmScroll + (y - listY) / MENU_ITEM_H;
        if (idx >= 0 && idx < (int)bmarks.size()) {
            if (x > W - 80) {
                // Tap on "x" → remove bookmark
                reader.removeBookmark(idx);
                needsRedraw = true;
            } else {
                // Tap bookmark → jump to it
                // Show loading indicator immediately so the user knows the device
                // is working (ZIP decompression can take several seconds).
                display_fill_screen(15);
                const char* loadMsg = "Loading chapter...";
                int loadW = display_text_width(loadMsg);
                display_draw_text((W - loadW) / 2, H / 2, loadMsg, 6);
                display_update_fast();

                readerFastRefresh = false;
                readerChapterJump = true;
                readerPageTurnsSinceFull = 0;
                if (!reader.jumpToBookmark(idx)) {
                    Serial.println("Bookmark jump failed, staying in reader");
                }
                appState = STATE_READER;
                needsRedraw = true;
            }
        }
    }
}

static void handleSettingsTouch(int x, int y) {
    // Footer → back
    if (y > H - FOOTER_HEIGHT) {
        settings_save();
        readerFastRefresh = false;
        // If we were reading a book, go back to reader, otherwise library
        if (reader.getTitle().length() > 0) {
            // Re-paginate in case font size changed, but preserve reading position.
            // jumpToChapter() resets page to 0, so save/restore it manually.
            int savedChapter = reader.getCurrentChapter();
            int savedPage = reader.getCurrentPage();
            reader.recalculateLayout();
            reader.jumpToChapter(savedChapter);
            reader.restorePage(savedPage);
            appState = STATE_READER;
        } else {
            appState = STATE_LIBRARY;
        }
        needsRedraw = true;
        return;
    }

    Settings& s = settings_get();
    int rowY = HEADER_HEIGHT + MARGIN_Y + 10;

    // Determine which row was tapped
    int row = (y - rowY) / SETTINGS_ROW_H;
    bool rightSide = (x > W / 2);

    // Row 0 = Font Size, Row 1 = font preview (no action), rest shifted +1
    if (row >= 0 && row < 12) {
        switch (row) {
            case 0: // Font Size — also update display font for live preview
                if (rightSide) {
                    s.fontSize = (s.fontSize + 1) % 3;
                } else {
                    s.fontSize = (s.fontSize + 2) % 3;
                }
                display_set_font_size(s.fontSize);
                break;
            case 1: // Font preview row — no action
                break;
            case 2: { // Sleep Timeout
                int idx = findSleepIdx();
                if (rightSide) {
                    idx = (idx + 1) % NUM_SLEEP_OPTIONS;
                } else {
                    idx = (idx + NUM_SLEEP_OPTIONS - 1) % NUM_SLEEP_OPTIONS;
                }
                s.sleepTimeoutMin = sleepOptions[idx];
                break;
            }
            case 3: // Sleep now
                enterDeepSleep();
                return;
            case 4: { // Cleanup Refresh cadence
                int idx = findRefreshIdx();
                if (rightSide) {
                    idx = (idx + 1) % NUM_REFRESH_OPTIONS;
                } else {
                    idx = (idx + NUM_REFRESH_OPTIONS - 1) % NUM_REFRESH_OPTIONS;
                }
                s.refreshEveryPages = refreshOptions[idx];
                break;
            }
            case 5: // Library View
                s.libraryViewMode = (s.libraryViewMode + 1) % 2;
                if (s.libraryViewMode == 1) {
                    s.posterShowCovers = true;
                }
                cover_cache_clear();
                break;
            case 6: // WiFi Upload
                appState = STATE_WIFI;
                wifi_upload_start();
                needsRedraw = true;
                return;
            case 7: // Poster Covers toggle
                s.posterShowCovers = !s.posterShowCovers;
                cover_cache_clear();
                break;
            case 8: // WiFi SSID (display only)
                break;
            case 9: // Page Numbers toggle
                s.showPageNumbers = !s.showPageNumbers;
                break;
            case 10: // Battery Display toggle
                s.showBattery = !s.showBattery;
                break;
            case 11: // Firmware Update — enter OTA check screen
                otaPhase = OTA_CHECKING;
                otaUpdateAvailable = false;
                otaLatestVersion = "";
                appState = STATE_OTA_CHECK;
                needsRedraw = true;
                return;
        }
        needsRedraw = true;
        return;
    }

    // Reset Defaults row (below the 12 settings rows including preview)
    int resetY = rowY + SETTINGS_ROW_H * 12 + 20;
    if (y >= resetY && y < resetY + FONT_H + 10) {
        settings_set_default();
        needsRedraw = true;
    }
}

static void handleWifiTouch(int x, int y) {
    (void)x; (void)y;
    wifi_upload_stop();
    books = library_scan();
    cover_cache_clear();
    appState = STATE_LIBRARY;
    needsRedraw = true;
}

// ═══════════════════════════════════════════════════════════════════
// Deep sleep
// ═══════════════════════════════════════════════════════════════════

static void enterDeepSleep(bool triggeredByButton) {
    Serial.println("Entering deep sleep...");

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
        if (appState == STATE_MENU || appState == STATE_TOC || appState == STATE_BOOKMARKS) {
            resumeState = (int)STATE_READER;
        }
        // Transient states resume to library
        if (appState == STATE_WIFI || appState == STATE_OTA_CHECK || appState == STATE_SETTINGS) {
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

        Serial.printf("Sleep: saved state=%d libScrl=%d book=%s\n",
            resumeState, libraryScroll, reader.getFilepath().c_str());
        prefs.end();
    }

    // If sleep was triggered by a physical button, wait for ALL buttons to
    // release first; otherwise the device can appear to restart immediately
    // because the wake condition is still asserted.
    if (triggeredByButton) {
        unsigned long waitStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW
               && millis() - waitStart < 2000) {
            delay(10);
        }
        delay(60);
    }

    bool showedSleepImage = sleep_image_show_next();
    if (!showedSleepImage) {
        display_fill_screen(15);
        const char* msg = "Sleeping...";
        int mw = display_text_width(msg);
        display_draw_text((W - mw) / 2, H / 2, msg, 6);
        display_update_sleep();
    }

    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════
// Arduino setup/loop
// ═══════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== T5 E-Reader Firmware (Portrait) ===");

    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    bool wakingFromSleep = (wakeReason == ESP_SLEEP_WAKEUP_EXT1 ||
                            wakeReason == ESP_SLEEP_WAKEUP_TIMER);
    Serial.printf("Wakeup cause: %d (fromSleep=%d)\n", (int)wakeReason, wakingFromSleep);

    // Top button is sleep / wake; middle button is for page turns.
    // Both are active LOW.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    // PAGE_BUTTON_PIN removed — top button (GPIO 21) handles all input

    display_init();

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

    battery_init();
    Serial.printf("Battery: %.2fV (%d%%)\n", battery_voltage(), battery_percent());

    if (!touch_init()) {
        Serial.println("WARNING: Touch not available");
    }

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

    settings_init();
    display_set_font_size(settings_get().fontSize);
    books = library_scan();
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
                    // Book opened successfully — reset crash guard
                    Preferences okPrefs;
                    okPrefs.begin("ereader", false);
                    okPrefs.putInt("crashCount", 0);
                    okPrefs.end();

                    appState = STATE_READER;
                    readerFastRefresh = false;
                    readerForceFullRefresh = true;
                    readerPageTurnsSinceFull = 0;
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

        // Draw the restored screen immediately after the wake banner so the
        // user gets instant acknowledgement first, then the restored content.
        firstLibraryDraw = true;
        needsRedraw = true;
        if (appState == STATE_READER) {
            drawReaderScreen();
        } else {
            drawLibraryScreen();
        }
        needsRedraw = false;
        Serial.println("Wake: immediate screen draw complete");
    } else {
        appState = STATE_LIBRARY;
        needsRedraw = true;
    }

    lastActivity = millis();

    Serial.println("Setup complete");
}

// Helper: advance or go back one page via the physical button
static void buttonPageForward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.nextPage()) {
            readerFastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerChapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !books.empty()) {
        const Settings& s = settings_get();
        int listStartY = HEADER_HEIGHT + MARGIN_Y;
        if (library_find_current_book(books) >= 0) listStartY += FONT_H + 20;
        int itemsPerPage;
        if (s.libraryViewMode == 1) {
            int posterH = 310;
            int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
            itemsPerPage = rowsVisible * 2;
        } else {
            itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
        }
        if (libraryScroll + itemsPerPage < (int)books.size()) {
            libraryScroll += itemsPerPage;
            needsRedraw = true;
        }
    }
}

static void buttonPageBackward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.prevPage()) {
            readerFastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerChapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !books.empty()) {
        const Settings& s = settings_get();
        int listStartY = HEADER_HEIGHT + MARGIN_Y;
        if (library_find_current_book(books) >= 0) listStartY += FONT_H + 20;
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
    } else if (btnPressed && btnWasPressed) {
        // Still held — check for long-press sleep trigger
        unsigned long heldMs = millis() - btnDownTime;
        if (heldMs >= BUTTON_POWER_MS) {
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
    } else if (!currentTouch && lastTouchState) {
        // Touch up — process tap or swipe
        lastActivity = millis();

        if (!touchHandled) {
            int tx = lastTouchPt.x;
            int ty = lastTouchPt.y;
            int dx = currentPt.x - lastTouchPt.x;
            int dy = currentPt.y - lastTouchPt.y;
            int absDx = abs(dx);
            int absDy = abs(dy);
            unsigned long duration = millis() - touchDownTime;
            bool isLongPress = (duration >= LONG_PRESS_MS);

            // Detect horizontal swipe in reader and library modes
            bool swipeHandled = false;
            if (absDx > 60 && absDy < 80) {
                if (appState == STATE_READER) {
                    if (dx < 0) {
                        // Swipe left → next page
                        if (reader.nextPage()) {
                            readerFastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerChapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        // Swipe right → prev page
                        if (reader.prevPage()) {
                            readerFastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerChapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    }
                } else if (appState == STATE_LIBRARY && !books.empty()) {
                    // Library swipe: compute items per page for current view mode
                    const Settings& s = settings_get();
                    int itemsPerPage;
                    if (s.libraryViewMode == 1) {
                        // Poster view
                        int listStartY = HEADER_HEIGHT + MARGIN_Y;
                        if (library_find_current_book(books) >= 0) listStartY += FONT_H + 20;
                        int posterH = 310;
                        int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
                        itemsPerPage = rowsVisible * 2;
                    } else {
                        // List view
                        int listStartY = HEADER_HEIGHT + MARGIN_Y;
                        if (library_find_current_book(books) >= 0) listStartY += FONT_H + 20;
                        itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
                    }
                    if (dx < 0) {
                        // Swipe left → next page of books
                        if (libraryScroll + itemsPerPage < (int)books.size()) {
                            libraryScroll += itemsPerPage;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        // Swipe right → prev page of books
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
                    case STATE_TOC:       handleTocTouch(tx, ty);               break;
                    case STATE_BOOKMARKS: handleBookmarksTouch(tx, ty);         break;
                    case STATE_SETTINGS:  handleSettingsTouch(tx, ty);          break;
                    case STATE_OTA_CHECK: handleOtaTouch(tx, ty);              break;
                    case STATE_WIFI:      handleWifiTouch(tx, ty);              break;
                    default: break;
                }
            }
        }
    }
    lastTouchState = currentTouch;

    // Redraw if needed
    if (needsRedraw) {
        switch (appState) {
            case STATE_LIBRARY:   drawLibraryScreen();   break;
            case STATE_READER:    drawReaderScreen();    break;
            case STATE_MENU:      drawMenuOverlay();     break;
            case STATE_TOC:       drawTocScreen();       break;
            case STATE_BOOKMARKS: drawBookmarksScreen(); break;
            case STATE_SETTINGS:  drawSettingsScreen();  break;
            case STATE_OTA_CHECK: drawOtaScreen();       break;
            case STATE_WIFI:      drawWifiScreen();      break;
            default: break;
        }
    }

    // Pre-caching removed — lines are stored on SD card now

    // Deep sleep check
    unsigned long sleepMs = (unsigned long)settings_get().sleepTimeoutMin * 60UL * 1000UL;
    if (millis() - lastActivity > sleepMs) {
        enterDeepSleep();
    }

    delay(TOUCH_POLL_MS);
}
