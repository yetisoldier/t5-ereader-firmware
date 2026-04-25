#include "ui_reader.h"
#include "../display.h"
#include "../settings.h"
#include "../battery.h"
#include "../inline_image.h"
#include "../reader.h"
#include "config.h"

// ─── Layout constants (matching main.cpp) ───────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;  // UI font height (medium FiraSans advance_y)

// Touch zones for reader (portrait)
static const int TOUCH_LEFT  = W / 3;
static const int TOUCH_RIGHT = W * 2 / 3;

// Menu/list item height
static const int MENU_ITEM_H = FONT_H + 24;

// Menu item Y positions for touch detection
static const int MENU_START_Y = 280;  // dropped ~8mm to avoid overlapping header
static const int LIST_NAV_Y = H - FOOTER_HEIGHT - FONT_H - 8;

static bool s_gotoPercentMode = false;
static int s_gotoValue = 1;

static String formatRemainingTime(uint32_t ms) {
    if (ms == 0) return "learning...";
    uint32_t minutes = (ms + 59999UL) / 60000UL;
    if (minutes < 1) minutes = 1;
    if (minutes >= 60) {
        uint32_t hours = minutes / 60;
        uint32_t mins = minutes % 60;
        char buf[24];
        snprintf(buf, sizeof(buf), "~%luh %lum", (unsigned long)hours, (unsigned long)mins);
        return String(buf);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "~%lu min", (unsigned long)minutes);
    return String(buf);
}

static void adjustGotoValue(BookReader& reader, int delta) {
    if (s_gotoPercentMode) {
        s_gotoValue += delta;
        if (s_gotoValue < 0) s_gotoValue = 0;
        if (s_gotoValue > 100) s_gotoValue = 100;
    } else {
        s_gotoValue += delta;
        int maxPage = reader.getApproxBookPageCount();
        if (s_gotoValue < 1) s_gotoValue = 1;
        if (s_gotoValue > maxPage) s_gotoValue = maxPage;
    }
}

// ─── Shared helpers (declared in main.cpp, linked) ──────────────────
extern void drawHeader(const char* title, bool showBattery);
extern void drawBottomBar(const char* label);

// ─── Callbacks into main.cpp state ──────────────────────────────────
extern void setNeedsRedraw(bool val);

// ═══════════════════════════════════════════════════════════════════
// Reader screen
// ═══════════════════════════════════════════════════════════════════

void ui_reader_draw(BookReader& reader, ReaderRefreshState& refresh) {
    const Settings& s = settings_get();
    int level = s.fontSizeLevel;
    if (level < 0) level = 0;
    if (level >= FONT_SIZE_LEVEL_COUNT) level = FONT_SIZE_LEVEL_COUNT - 1;
    display_set_font(level, s.serifFont);
    int marginX = FONT_MARGIN_X_VALUES[level];
    uint8_t spacingLevel = s.lineSpacingLevel;
    if (spacingLevel >= LINE_SPACING_LEVEL_COUNT) spacingLevel = 2;

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
    int lineHeight = (bodyFontH * LINE_SPACING_PCT[spacingLevel] + 99) / 100;
    if (lineHeight < bodyFontH) lineHeight = bodyFontH;
    int lineSpacing = lineHeight - bodyFontH;
    int lineH = bodyFontH + lineSpacing;
    int y = bodyTop + fontAscender;  // first baseline
    int bodyBottom = H - FOOTER_HEIGHT - 2;  // minimal guard above the footer area

    if (lines.empty()) {
        display_draw_text(marginX, y, "[Page content unavailable]", 6);
    } else {
        for (const auto& line : lines) {
            if (y > bodyBottom) break;

            if (inline_image_is_marker(line)) {
                // Temporary safety workaround: skip inline image rendering.
                // The linecache already reserves vertical space via continuation markers,
                // so advancing by one line here preserves layout well enough for debugging.
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
    if (refresh.forceFullRefresh) {
        // Full refresh to cleanly replace sleep image on wake resume
        display_update();
        refresh.forceFullRefresh = false;
        refresh.pageTurnsSinceFull = 0;
    } else if (refresh.fastRefresh) {
        int refreshInterval = settings_get().refreshEveryPages;
        if (refreshInterval < 1) refreshInterval = 4;
        bool strongCleanup = (refresh.pageTurnsSinceFull + 1 >= refreshInterval);

        if (strongCleanup) {
            display_update_medium();
            refresh.pageTurnsSinceFull = 0;
        } else {
            display_update_fast();
            refresh.pageTurnsSinceFull++;
        }
    } else if (refresh.chapterJump) {
        display_update_fast();
        refresh.pageTurnsSinceFull = 0;
        refresh.chapterJump = false;
    } else {
        display_update_fast();
        refresh.pageTurnsSinceFull = 0;
    }
    setNeedsRedraw(false);
}

// ═══════════════════════════════════════════════════════════════════
// Reader touch
// ═══════════════════════════════════════════════════════════════════

AppState ui_reader_touch(int x, int y, bool isLongPress,
                         BookReader& reader, ReaderRefreshState& refresh) {
    (void)y;  // not used for zone detection (horizontal only)

    // Long-press center zone → add bookmark
    if (isLongPress && x >= TOUCH_LEFT && x <= TOUCH_RIGHT) {
        reader.addBookmark();
        refresh.fastRefresh = false;
        setNeedsRedraw(true);
        return STATE_READER;
    }

    // Left zone → prev page
    if (x < TOUCH_LEFT) {
        if (reader.prevPage()) {
            // Use medium refresh for chapter transitions (heavier processing)
            refresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) refresh.chapterJump = true;
            setNeedsRedraw(true);
        }
        return STATE_READER;
    }

    // Right zone → next page
    if (x > TOUCH_RIGHT) {
        if (reader.nextPage()) {
            refresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) refresh.chapterJump = true;
            setNeedsRedraw(true);
        }
        return STATE_READER;
    }

    // Center short tap → show menu
    refresh.fastRefresh = false;
    setNeedsRedraw(true);
    return STATE_MENU;
}

// ═══════════════════════════════════════════════════════════════════
// Menu overlay
// ═══════════════════════════════════════════════════════════════════

void ui_reader_menu_draw(BookReader& reader) {
    display_fill_screen(15);

    // Title — truncate by pixel width to fit screen
    String title = reader.getTitle();
    int maxMenuTitleW = W - MARGIN_X * 2;
    while (title.length() > 3 &&
           display_text_width(title.c_str()) > maxMenuTitleW) {
        title = title.substring(0, title.length() - 4) + "...";
    }
    String titleLine = title;
    if (reader.getAuthor().length() > 0) {
        titleLine = reader.getAuthor() + " • " + title;
    }
    while (titleLine.length() > 3 && display_text_width(titleLine.c_str()) > maxMenuTitleW) {
        titleLine = titleLine.substring(0, titleLine.length() - 4) + "...";
    }
    int tw = display_text_width(titleLine.c_str());
    display_draw_text((W - tw) / 2, 80, titleLine.c_str(), 0);

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

    String chapterEta = String("Chapter: ") + formatRemainingTime(reader.getEstimatedChapterRemainingMs()) + " remaining";
    int cew = display_text_width(chapterEta.c_str());
    display_draw_text((W - cew) / 2, 195, chapterEta.c_str(), 8);

    String bookEta = String("Book: ") + formatRemainingTime(reader.getEstimatedBookRemainingMs()) + " remaining";
    int bew = display_text_width(bookEta.c_str());
    display_draw_text((W - bew) / 2, 225, bookEta.c_str(), 8);

    display_draw_hline(MARGIN_X, 255, W - MARGIN_X * 2, 10);

    // Menu items
    int y = MENU_START_Y + 40;
    const int indent = MARGIN_X + 30;

    if (reader.hasNavigationHistory()) {
        display_draw_text(indent, y, "Back", 0);
        display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
        y += MENU_ITEM_H;
    }

    display_draw_text(indent, y, "Go to...", 0);
    display_draw_hline(MARGIN_X, y + 18, W - MARGIN_X * 2, 12);
    y += MENU_ITEM_H;

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
    setNeedsRedraw(false);
}

AppState ui_reader_menu_touch(int x, int y, BookReader& reader,
                              ReaderRefreshState& refresh) {
    (void)x;
    // Text is drawn at baseline Y — visible glyphs extend ~FONT_H above baseline.
    // Shift touch zone up so it aligns with what the user sees.
    int zoneTop = MENU_START_Y + 40 - FONT_H;
    int row = (y - zoneTop) / MENU_ITEM_H;
    int rowCount = reader.hasNavigationHistory() ? 6 : 5;

    if (y >= zoneTop && y < zoneTop + MENU_ITEM_H * rowCount) {
        refresh.fastRefresh = false;
        int base = 0;
        if (reader.hasNavigationHistory()) {
            if (row == 0) {
                reader.goBackInHistory();
                setNeedsRedraw(true);
                return STATE_READER;
            }
            base = 1;
        }
        int action = row - base;
        switch (action) {
            case 0: // Go to
                s_gotoPercentMode = false;
                s_gotoValue = reader.getApproxBookPage();
                setNeedsRedraw(true);
                return STATE_GOTO;
            case 1: // Table of Contents
                setNeedsRedraw(true);
                return STATE_TOC;
            case 2: // Bookmarks
                setNeedsRedraw(true);
                return STATE_BOOKMARKS;
            case 3: // Settings
                setNeedsRedraw(true);
                return STATE_SETTINGS;
            case 4: // Library
                reader.saveProgress();
                reader.closeBook();
                setNeedsRedraw(true);
                return STATE_LIBRARY;
        }
    }

    // Tap outside menu items → back to reader
    refresh.fastRefresh = false;
    setNeedsRedraw(true);
    return STATE_READER;
}

void ui_reader_goto_draw(BookReader& reader) {
    display_fill_screen(15);
    drawHeader("Go to...", true);

    String subtitle = s_gotoPercentMode ? "Percentage" : "Approximate page";
    int sw = display_text_width(subtitle.c_str());
    display_draw_text((W - sw) / 2, 130, subtitle.c_str(), 8);

    char valueBuf[32];
    if (s_gotoPercentMode) {
        snprintf(valueBuf, sizeof(valueBuf), "%d%%", s_gotoValue);
    } else {
        snprintf(valueBuf, sizeof(valueBuf), "%d / %d", s_gotoValue, reader.getApproxBookPageCount());
    }
    int vw = display_text_width(valueBuf);
    display_draw_text((W - vw) / 2, 220, valueBuf, 0);

    display_draw_text(MARGIN_X + 35, 320, "-10", 3);
    display_draw_text(W / 2 - 55, 320, "-1", 3);
    display_draw_text(W / 2 + 25, 320, "+1", 3);
    display_draw_text(W - MARGIN_X - 90, 320, "+10", 3);
    display_draw_hline(MARGIN_X, 340, W - MARGIN_X * 2, 12);

    const char* modeLbl = s_gotoPercentMode ? "[ Percent ]" : "[ Page ]";
    int mw = display_text_width(modeLbl);
    display_draw_text((W - mw) / 2, 420, modeLbl, 6);

    const char* goLbl = "[ Jump ]";
    int gw = display_text_width(goLbl);
    display_draw_text((W - gw) / 2, 520, goLbl, 4);

    drawBottomBar("[ Back to Reading ]");
    display_update();
    setNeedsRedraw(false);
}

AppState ui_reader_goto_touch(int x, int y, BookReader& reader,
                              ReaderRefreshState& refresh) {
    if (y > H - FOOTER_HEIGHT) {
        refresh.fastRefresh = false;
        setNeedsRedraw(true);
        return STATE_READER;
    }

    if (y >= 280 && y <= 350) {
        if (x < W / 4) adjustGotoValue(reader, -10);
        else if (x < W / 2) adjustGotoValue(reader, -1);
        else if (x < (W * 3) / 4) adjustGotoValue(reader, 1);
        else adjustGotoValue(reader, 10);
        setNeedsRedraw(true);
        return STATE_GOTO;
    }

    if (y >= 380 && y <= 450) {
        s_gotoPercentMode = !s_gotoPercentMode;
        s_gotoValue = s_gotoPercentMode ? reader.getApproxBookPercent() : reader.getApproxBookPage();
        setNeedsRedraw(true);
        return STATE_GOTO;
    }

    if (y >= 480 && y <= 560) {
        bool jumped = s_gotoPercentMode
            ? reader.jumpToBookProgressPercent(s_gotoValue, true)
            : reader.jumpToApproxBookPage(s_gotoValue, true);
        refresh.fastRefresh = false;
        refresh.chapterJump = jumped;
        refresh.pageTurnsSinceFull = 0;
        setNeedsRedraw(true);
        return STATE_READER;
    }

    return STATE_GOTO;
}

// ═══════════════════════════════════════════════════════════════════
// TOC screen
// ═══════════════════════════════════════════════════════════════════

void ui_reader_toc_draw(BookReader& reader, int& tocScroll) {
    display_fill_screen(15);
    drawHeader("Table of Contents", true);

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
    setNeedsRedraw(false);
}

AppState ui_reader_toc_touch(int x, int y, BookReader& reader,
                             int& tocScroll, ReaderRefreshState& refresh) {
    // Footer → back to reader
    if (y > H - FOOTER_HEIGHT) {
        refresh.fastRefresh = false;
        setNeedsRedraw(true);
        return STATE_READER;
    }

    int listY = HEADER_HEIGHT + MARGIN_Y;
    int totalEntries = reader.getTocCount();
    int maxVisible = (H - listY - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;

    if (totalEntries > maxVisible && y >= LIST_NAV_Y - 8 && y < H - FOOTER_HEIGHT) {
        if (x < W / 3 && tocScroll > 0) {
            tocScroll = max(0, tocScroll - maxVisible);
            setNeedsRedraw(true);
            return STATE_TOC;
        }
        if (x > (W * 2) / 3 && tocScroll + maxVisible < totalEntries) {
            tocScroll = min(max(0, totalEntries - maxVisible), tocScroll + maxVisible);
            setNeedsRedraw(true);
            return STATE_TOC;
        }
    }

    if (y > listY) {
        int idx = tocScroll + (y - listY) / MENU_ITEM_H;
        if (idx >= 0 && idx < totalEntries) {
            int chapterIndex = reader.getTocChapterIndex(idx);
            if (chapterIndex < 0) return STATE_TOC;

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

            refresh.fastRefresh = false;
            refresh.chapterJump = true;
            refresh.pageTurnsSinceFull = 0;
            reader.jumpToChapter(chapterIndex, true);

            setNeedsRedraw(true);
            return STATE_READER;
        }
    }

    return STATE_TOC;
}

// ═══════════════════════════════════════════════════════════════════
// Bookmarks screen
// ═══════════════════════════════════════════════════════════════════

void ui_reader_bookmarks_draw(BookReader& reader, int& bmScroll) {
    display_fill_screen(15);
    drawHeader("Bookmarks", true);

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
    setNeedsRedraw(false);
}

AppState ui_reader_bookmarks_touch(int x, int y, BookReader& reader,
                                   int& bmScroll, ReaderRefreshState& refresh) {
    // Footer → back
    if (y > H - FOOTER_HEIGHT) {
        refresh.fastRefresh = false;
        setNeedsRedraw(true);
        return STATE_READER;
    }

    const auto& bmarks = reader.getBookmarks();
    if (bmarks.empty()) return STATE_BOOKMARKS;

    int listY = HEADER_HEIGHT + MARGIN_Y;
    int maxVisible = (H - listY - FOOTER_HEIGHT - MARGIN_Y) / MENU_ITEM_H;

    if ((int)bmarks.size() > maxVisible && y >= LIST_NAV_Y - 8 && y < H - FOOTER_HEIGHT) {
        if (x < W / 3 && bmScroll > 0) {
            bmScroll = max(0, bmScroll - maxVisible);
            setNeedsRedraw(true);
            return STATE_BOOKMARKS;
        }
        if (x > (W * 2) / 3 && bmScroll + maxVisible < (int)bmarks.size()) {
            bmScroll = min(max(0, (int)bmarks.size() - maxVisible), bmScroll + maxVisible);
            setNeedsRedraw(true);
            return STATE_BOOKMARKS;
        }
    }

    if (y > listY) {
        int idx = bmScroll + (y - listY) / MENU_ITEM_H;
        if (idx >= 0 && idx < (int)bmarks.size()) {
            if (x > W - 80) {
                // Tap on "x" → remove bookmark
                reader.removeBookmark(idx);
                setNeedsRedraw(true);
                return STATE_BOOKMARKS;
            } else {
                // Tap bookmark → jump to it
                // Show loading indicator immediately so the user knows the device
                // is working (ZIP decompression can take several seconds).
                display_fill_screen(15);
                const char* loadMsg = "Loading chapter...";
                int loadW = display_text_width(loadMsg);
                display_draw_text((W - loadW) / 2, H / 2, loadMsg, 6);
                display_update_fast();

                refresh.fastRefresh = false;
                refresh.chapterJump = true;
                refresh.pageTurnsSinceFull = 0;
                if (!reader.jumpToBookmark(idx)) {
                    Serial.println("Bookmark jump failed, staying in reader");
                }
                setNeedsRedraw(true);
                return STATE_READER;
            }
        }
    }

    return STATE_BOOKMARKS;
}
