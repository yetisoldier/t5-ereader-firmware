#include "ui_library.h"
#include "../display.h"
#include "../settings.h"
#include "../battery.h"
#include "../cover_renderer.h"
#include "../opds_store.h"
#include "../reader.h"
#include "config.h"
#include <algorithm>

// Layout constants (matching main.cpp)
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;  // UI font height (medium FiraSans advance_y)
static const int BOOK_ITEM_H = FONT_H * 2 + 12;  // title + info + padding

static const char* filterNames[] = {"ALL", "NEW", "READING", "DONE"};
static const int NUM_FILTERS = 4;
static const int FILTER_TAB_H = 44;
static const char* librarySortNames[] = {"Title", "Author", "Recent", "Size"};

extern BookReader reader;

static void wrapPosterTitle(const String& title, int maxWidth, int maxLines, std::vector<String>& lines) {
    lines.clear();
    String remaining = title;
    while (remaining.length() > 0 && (int)lines.size() < maxLines) {
        int len = 1;
        String candidate = remaining.substring(0, len);
        while (len < (int)remaining.length() && display_text_width(candidate.c_str()) < maxWidth) {
            len++;
            candidate = remaining.substring(0, len);
        }
        if (len >= (int)remaining.length()) {
            lines.push_back(remaining);
            break;
        }
        while (len > 1 && remaining.charAt(len - 1) != ' ') len--;
        if (len == 1) len = remaining.length();
        lines.push_back(remaining.substring(0, len));
        remaining = remaining.substring(len);
        while (remaining.length() > 0 && remaining.charAt(0) == ' ') {
            remaining = remaining.substring(1);
        }
    }
}

static void drawHeader(const char* title, bool showBattery = true) {
    display_draw_filled_rect(0, 0, W, HEADER_HEIGHT, 2);
    display_draw_text(MARGIN_X, HEADER_HEIGHT - 18, title, 15);

    char sortStr[32];
    snprintf(sortStr, sizeof(sortStr), "Sort: %s", librarySortNames[min((int)settings_get().librarySortOrder, 3)]);
    int sortW = display_text_width(sortStr);
    int sortX = (W - sortW) / 2;
    if (sortX > MARGIN_X + 120) {
        display_draw_text(sortX, HEADER_HEIGHT - 18, sortStr, 12);
    }

    if (showBattery && settings_get().showBattery) {
        char battStr[16];
        snprintf(battStr, sizeof(battStr), "%d%%", battery_percent());
        int bw = display_text_width(battStr);
        display_draw_text(W - MARGIN_X - bw, HEADER_HEIGHT - 18, battStr, 15);
    }

    display_draw_hline(0, HEADER_HEIGHT, W, 0);
}

static void drawPosterNoCoverTile(const BookInfo& book, int x, int y, int w, int h) {
    display_draw_filled_rect(x, y, w, h, 15);
    display_draw_rect(x, y, w, h, 8);

    display_draw_filled_rect(x + 10, y + 10, w - 20, 20, 13);
    const char* band = "BOOK";
    int bandW = display_text_width(band);
    display_draw_text(x + (w - bandW) / 2, y + 24, band, 6);

    std::vector<String> titleLines;
    wrapPosterTitle(book.title, w - 24, 4, titleLines);

    int titleBlockTop = y + 62;
    int lineStep = FONT_H - 6;
    int availH = h - 110;
    int totalHeight = (int)titleLines.size() * lineStep;
    int ty = titleBlockTop + max(0, (availH - totalHeight) / 2);
    for (const auto& line : titleLines) {
        int lw = display_text_width(line.c_str());
        display_draw_text(x + (w - lw) / 2, ty, line.c_str(), 0);
        ty += lineStep;
    }

    String info = book.filepath;
    int ls = info.lastIndexOf('/');
    if (ls >= 0) info = info.substring(ls + 1);
    if (info.length() > 32) info = info.substring(0, 29) + "...";
    int infoW = display_text_width(info.c_str());
    display_draw_text(x + (w - infoW) / 2, y + h - 38, info.c_str(), 8);

    if (book.author.length() > 0) {
        String author = book.author;
        while (author.length() > 3 && display_text_width(author.c_str()) > w - 24) {
            author = author.substring(0, author.length() - 4) + "...";
        }
        int authorW = display_text_width(author.c_str());
        display_draw_text(x + (w - authorW) / 2, y + h - 68, author.c_str(), 8);
    }

    if (book.hasProgress && book.totalChapters > 0) {
        int pct = (book.progressChapter * 100) / max(1, book.totalChapters);
        if (pct > 100) pct = 100;
        char pctStr[10];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        display_draw_text(x + 14, y + h - 18, pctStr, 4);
    }

    if (book.author.length() > 0) {
        String author = book.author;
        while (author.length() > 3 && display_text_width(author.c_str()) > w - 30) {
            author = author.substring(0, author.length() - 4) + "...";
        }
        int authorW = display_text_width(author.c_str());
        display_draw_text(x + (w - authorW) / 2, y + h - 48, author.c_str(), 8);
    }
}

static void drawDefaultPoster(BookInfo& book, int x, int y, int w, int h) {
    if (settings_get().posterShowCovers) {
        if (book.posterCoverFailed) {
            drawPosterNoCoverTile(book, x, y, w, h);
            return;
        }

        if (cover_can_render_poster(book) && cover_render_poster(book, x, y, w, h)) {
            return;
        }

        if (book.hasCover && book.coverPath.length() > 0) {
            book.posterCoverFailed = true;
            Serial.printf("Poster fallback: %s will use covers-off rendering (cover unavailable or failed: %s)\n",
                          book.filepath.c_str(), book.coverPath.c_str());
        }
        drawPosterNoCoverTile(book, x, y, w, h);
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
    int availH = h - 110;
    int totalHeight = (int)titleLines.size() * lineStep;
    int ty = titleBlockTop + max(0, (availH - totalHeight) / 2);
    for (const auto& line : titleLines) {
        int lw = display_text_width(line.c_str());
        display_draw_text(x + (w - lw) / 2, ty, line.c_str(), 0);
        ty += lineStep;
    }

    if (book.hasProgress && book.totalChapters > 0) {
        int pct = (book.progressChapter * 100) / max(1, book.totalChapters);
        if (pct > 100) pct = 100;
        char pctStr[10];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        display_draw_text(x + 14, y + h - 18, pctStr, 4);
    }
}

static void drawFilterTabs(int activeFilter) {
    int tabY = HEADER_HEIGHT;
    display_draw_filled_rect(0, tabY, W, FILTER_TAB_H, 14);

    int tabW = W / NUM_FILTERS;
    for (int i = 0; i < NUM_FILTERS; i++) {
        int tx = i * tabW;
        if (i == activeFilter) {
            display_draw_filled_rect(tx + 2, tabY + 2, tabW - 4, FILTER_TAB_H - 4, 15);
            display_draw_hline(tx + 2, tabY + FILTER_TAB_H - 2, tabW - 4, 0);
        }
        int tw = display_text_width(filterNames[i]);
        uint8_t color = (i == activeFilter) ? 0 : 6;
        display_draw_text(tx + (tabW - tw) / 2, tabY + FILTER_TAB_H - 10, filterNames[i], color);
    }
    display_draw_hline(0, tabY + FILTER_TAB_H, W, 10);
}

static void drawBottomBarSplit(const char* left, const char* right) {
    int barY = H - FOOTER_HEIGHT;
    display_draw_filled_rect(0, barY, W, FOOTER_HEIGHT, 13);
    display_draw_hline(0, barY, W, 8);
    display_draw_filled_rect(W / 2 - 1, barY + 4, 2, FOOTER_HEIGHT - 8, 8);
    int lw = display_text_width(left);
    display_draw_text(W / 4 - lw / 2, barY + FOOTER_HEIGHT - 12, left, 3);
    int rw = display_text_width(right);
    display_draw_text(W * 3 / 4 - rw / 2, barY + FOOTER_HEIGHT - 12, right, 3);
}

void ui_library_draw(
    std::vector<BookInfo>& books,
    int& scroll,
    int filter,
    const std::vector<int>& filteredIndices,
    bool& firstDraw
) {
    display_set_font_size(2);
    display_fill_screen(15);
    drawHeader("Library");
    drawFilterTabs(filter);

    const Settings& s = settings_get();
    const auto& visibleIdx = filteredIndices;
    int numVisible = (int)visibleIdx.size();

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
    } else if (numVisible == 0) {
        int cy = H / 2 - 20;
        const char* msg = "No books match filter";
        int mw = display_text_width(msg);
        display_draw_text((W - mw) / 2, cy, msg, 6);
    } else if (s.libraryViewMode == 1) {
        int y = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;

        int currentIdx = library_find_current_book(books);
        if (currentIdx >= 0 && filter == FILTER_ALL) {
            int bannerH = FONT_H + 16;
            int bannerY = y + bannerH / 2 + 2;  // Vertically centered baseline
            display_draw_filled_rect(0, y, W, bannerH, 14);
            display_draw_text(MARGIN_X, bannerY, "Continue Reading:", 4);

            String contTitle = books[currentIdx].title;
            int labelW = display_text_width("Continue Reading: ");
            int maxTitleW = W - MARGIN_X * 2 - labelW;
            while (contTitle.length() > 3 && display_text_width(contTitle.c_str()) > maxTitleW) {
                contTitle = contTitle.substring(0, contTitle.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X + labelW, bannerY, contTitle.c_str(), 0);
            display_draw_hline(0, y + bannerH, W, 10);
            y += bannerH + MARGIN_Y + 16;  // Extra padding before book grid
        }

        const int cols = 2;
        const int gap = 18;
        int posterW = (W - MARGIN_X * 2 - gap) / cols;
        int posterH = 310;
        int rowsVisible = max(1, (H - y - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
        int cardsPerPage = rowsVisible * cols;

        for (int vi = scroll; vi < numVisible && vi < scroll + cardsPerPage; vi++) {
            int bi = visibleIdx[vi];
            int rel = vi - scroll;
            int row = rel / cols;
            int col = rel % cols;
            int px = MARGIN_X + col * (posterW + gap);
            int py = y + row * (posterH + 14);
            drawDefaultPoster(books[bi], px, py, posterW, posterH);
        }

        int itemsOnPage = min(numVisible - scroll, cardsPerPage);
        int rowsOnPage = (itemsOnPage + cols - 1) / cols;
        int contentBottom = y + (rowsOnPage > 0 ? (rowsOnPage - 1) * (posterH + 14) + posterH : y);
        int footerTop = H - FOOTER_HEIGHT;
        int pageInfoY = (contentBottom + footerTop) / 2 + FONT_H / 4;

        if (numVisible > cardsPerPage) {
            bool hasPrev = (scroll > 0);
            bool hasNext = (scroll + cardsPerPage < numVisible);

            if (hasPrev) display_draw_text(MARGIN_X, pageInfoY, "< Prev", 4);
            if (hasNext) {
                const char* nextLbl = "Next >";
                int nw = display_text_width(nextLbl);
                display_draw_text(W - MARGIN_X - nw, pageInfoY, nextLbl, 4);
            }

            int totalPages = (numVisible + cardsPerPage - 1) / cardsPerPage;
            int curPage = scroll / max(1, cardsPerPage) + 1;
            char pageStr[32];
            snprintf(pageStr, sizeof(pageStr), "%d / %d", curPage, totalPages);
            int pw = display_text_width(pageStr);
            display_draw_text((W - pw) / 2, pageInfoY, pageStr, 4);
        }
    } else {
        int y = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;

        int currentIdx = library_find_current_book(books);
        if (currentIdx >= 0 && filter == FILTER_ALL) {
            int bannerH = FONT_H + 16;
            int bannerY = y + bannerH / 2 + 2;  // Vertically centered baseline
            display_draw_filled_rect(0, y, W, bannerH, 14);
            display_draw_text(MARGIN_X, bannerY, "Continue Reading:", 4);

            String contTitle = books[currentIdx].title;
            int labelW = display_text_width("Continue Reading: ");
            int maxTitleW = W - MARGIN_X * 2 - labelW;
            while (contTitle.length() > 3 && display_text_width(contTitle.c_str()) > maxTitleW) {
                contTitle = contTitle.substring(0, contTitle.length() - 4) + "...";
            }
            display_draw_text(MARGIN_X + labelW, bannerY, contTitle.c_str(), 0);
            display_draw_hline(0, y + bannerH, W, 10);
            y += bannerH + MARGIN_Y + 16;  // Extra padding before book list
        }

        int maxVis = (H - y - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;

        for (int vi = scroll; vi < numVisible && vi < scroll + maxVis; vi++) {
            int bi = visibleIdx[vi];
            const BookInfo& book = books[bi];
            int itemY = y + (vi - scroll) * BOOK_ITEM_H;

            display_draw_filled_rect(MARGIN_X, itemY, W - MARGIN_X * 2, BOOK_ITEM_H - 4, 15);
            display_draw_rect(MARGIN_X, itemY, W - MARGIN_X * 2, BOOK_ITEM_H - 4, 8);

            String title = book.title;
            if (display_text_width(title.c_str()) > W - MARGIN_X * 2 - 80) {
                while (title.length() > 3 && display_text_width(title.c_str()) > W - MARGIN_X * 2 - 80) {
                    title = title.substring(0, title.length() - 4) + "...";
                }
            }
            display_draw_text(MARGIN_X + 8, itemY + 12, title.c_str(), 0);

            if (book.hasProgress && book.totalChapters > 0) {
                int pct = (book.progressChapter * 100) / max(1, book.totalChapters);
                if (pct > 100) pct = 100;
                char pctStr[10];
                snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
                int pw = display_text_width(pctStr);
                display_draw_text(W - MARGIN_X - pw - 8, itemY + 12, pctStr, 4);
            }

            String info = book.author;
            if (info.length() == 0 && book.totalChapters > 0) info = String(book.totalChapters) + " chapters";
            while (info.length() > 3 && display_text_width(info.c_str()) > W - MARGIN_X * 2 - 20) {
                info = info.substring(0, info.length() - 4) + "...";
            }
            if (info.length() > 0) display_draw_text(MARGIN_X + 8, itemY + FONT_H + 8, info.c_str(), 8);
        }

        int contentBottom = y + min(numVisible - scroll, maxVis) * BOOK_ITEM_H;
        int pageControlY = (contentBottom + (H - FOOTER_HEIGHT)) / 2 - FONT_H / 2;

        if (numVisible > maxVis) {
            bool hasPrev = (scroll > 0);
            bool hasNext = (scroll + maxVis < numVisible);

            if (hasPrev) display_draw_text(MARGIN_X, pageControlY, "< Prev", 4);
            if (hasNext) {
                const char* nextLbl = "Next >";
                int nw = display_text_width(nextLbl);
                display_draw_text(W - MARGIN_X - nw, pageControlY, nextLbl, 4);
            }

            int totalPages = (numVisible + maxVis - 1) / maxVis;
            int curPage = scroll / max(1, maxVis) + 1;
            char pageStr[32];
            snprintf(pageStr, sizeof(pageStr), "%d / %d", curPage, totalPages);
            int pw = display_text_width(pageStr);
            display_draw_text((W - pw) / 2, pageControlY, pageStr, 4);
        }
    }

    drawBottomBarSplit("[ Store ]", "[ Settings ]");

    // Pre-cache next page covers in poster view mode
    if (settings_get().libraryViewMode == 1 && !books.empty()) {
        const int cols = 2;
        const int gap = 18;
        int posterH = 310;
        int posterW = (W - MARGIN_X * 2 - gap) / cols;
        int rowsVisible = max(1, (H - HEADER_HEIGHT - FILTER_TAB_H - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
        int cardsPerPage = rowsVisible * cols;
        cover_precache_page(books, filteredIndices, scroll, cardsPerPage);
    }

    if (firstDraw) {
        firstDraw = false;
        display_update();
    } else {
        display_update_medium();
    }
}

AppState ui_library_touch(
    int x, int y,
    std::vector<BookInfo>& books,
    int& scroll,
    int& filter,
    std::vector<int>& filteredIndices
) {
    if (y > H - FOOTER_HEIGHT) {
        if (x < W / 2) {
            opds_store_init();
            return STATE_OPDS_BROWSE;
        }
        return STATE_SETTINGS;
    }

    if (y < HEADER_HEIGHT) {
        Settings& s = settings_get();
        s.librarySortOrder = (s.librarySortOrder + 1) % 4;
        library_sort(books);
        filteredIndices = library_filter(books, (LibraryFilter)filter);
        scroll = 0;
        settings_save();
        return STATE_LIBRARY;
    }

    if (y >= HEADER_HEIGHT && y < HEADER_HEIGHT + FILTER_TAB_H) {
        int tabW = W / NUM_FILTERS;
        int newFilter = x / tabW;
        if (newFilter >= 0 && newFilter < NUM_FILTERS && newFilter != filter) {
            filter = newFilter;
            scroll = 0;
            filteredIndices = library_filter(books, (LibraryFilter)filter);
        }
        return STATE_LIBRARY;
    }

    if (books.empty() || filteredIndices.empty()) return STATE_LIBRARY;

    const Settings& s = settings_get();
    int numVisible = (int)filteredIndices.size();

    int currentIdx = library_find_current_book(books);
    int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;

    if (currentIdx >= 0 && filter == FILTER_ALL) {
        int bannerBottom = listStartY + FONT_H + 16 + MARGIN_Y;
        if (y > listStartY && y < bannerBottom) {
            Serial.printf("Continue reading: %s\n", books[currentIdx].filepath.c_str());
            if (reader.openBook(books[currentIdx].filepath.c_str())) {
                return STATE_READER;
            }
            return STATE_LIBRARY;
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
                        int vi = scroll + row * cols + col;
                        if (vi >= 0 && vi < numVisible && vi < scroll + cardsPerPage) {
                            int bi = filteredIndices[vi];
                            Serial.printf("Opening book: %s\n", books[bi].filepath.c_str());
                            if (reader.openBook(books[bi].filepath.c_str())) return STATE_READER;
                            return STATE_LIBRARY;
                        }
                    }
                }
            }
        }

        int itemsOnPage = min(numVisible - scroll, cardsPerPage);
        int rowsOnPage = (itemsOnPage + cols - 1) / cols;
        int contentBottom = listStartY + (rowsOnPage > 0 ? (rowsOnPage - 1) * (posterH + 14) + posterH : 0);
        int pageControlY = (contentBottom + (H - FOOTER_HEIGHT)) / 2 - FONT_H / 2;

        if (numVisible > cardsPerPage && y >= pageControlY && y <= H - FOOTER_HEIGHT) {
            if (x < W / 3 && scroll > 0) {
                scroll -= cardsPerPage;
                if (scroll < 0) scroll = 0;
            } else if (x > W * 2 / 3 && scroll + cardsPerPage < numVisible) {
                scroll += cardsPerPage;
                if (scroll >= numVisible) scroll = max(0, numVisible - cardsPerPage);
            }
        }
    } else {
        int maxVis = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;

        int contentBottom = listStartY + min(numVisible - scroll, maxVis) * BOOK_ITEM_H;
        int pageControlY = (contentBottom + (H - FOOTER_HEIGHT)) / 2 - FONT_H / 2;

        if (numVisible > maxVis && y >= pageControlY && y <= H - FOOTER_HEIGHT) {
            if (x < W / 3 && scroll > 0) {
                scroll -= maxVis;
                if (scroll < 0) scroll = 0;
                return STATE_LIBRARY;
            } else if (x > W * 2 / 3 && scroll + maxVis < numVisible) {
                scroll += maxVis;
                if (scroll >= numVisible) scroll = max(0, numVisible - maxVis);
                return STATE_LIBRARY;
            }
        }

        if (y > listStartY) {
            int vi = scroll + (y - listStartY) / BOOK_ITEM_H;
            if (vi >= 0 && vi < numVisible && vi < scroll + maxVis) {
                int bi = filteredIndices[vi];
                Serial.printf("Opening book: %s\n", books[bi].filepath.c_str());
                if (reader.openBook(books[bi].filepath.c_str())) return STATE_READER;
            }
        }
    }

    return STATE_LIBRARY;
}
