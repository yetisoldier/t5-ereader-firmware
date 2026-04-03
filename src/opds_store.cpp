#include "opds_store.h"
#include "config.h"
#include "display.h"
#include "settings.h"
#include <WiFi.h>
#include <SD.h>

static OpdsStoreState _state;
static OpdsClient _client;
static std::vector<OpdsServer> _servers;
static bool _needsLibraryRefresh = false;

// Layout constants (portrait 540x960)
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int STORE_HEADER_H = 66;
static const int SERVER_TAB_H = 50;
static const int ENTRY_CARD_H = 100;
static const int STORE_FOOTER_H = 50;
static const int MARGIN = 30;
static const int FONT_H = 50;
static const int ENTRIES_PER_PAGE = 7;

// ─── Category browsing (Gutenberg) ──────────────────────────────────
// Since the device has no text keyboard, we offer genre quick-links
// that search Gutenberg's OPDS endpoint with predefined query terms.
struct CategoryDef {
    const char* label;
    const char* query;   // URL-encoded search term
};

static const CategoryDef GUTENBERG_CATEGORIES[] = {
    {"Adventure",  "adventure"},
    {"Fantasy",    "fantasy"},
    {"Mystery",    "mystery"},
    {"Romance",    "romance"},
    {"Sci-Fi",     "science+fiction"},
    {"History",    "history"},
    {"Poetry",     "poetry"},
    {"Children",   "children"},
};
static const int NUM_CATEGORIES = sizeof(GUTENBERG_CATEGORIES) / sizeof(GUTENBERG_CATEGORIES[0]);
static const int CATEGORY_ROW_H = 42;
static int _categoryScroll = 0;           // first visible category index
static const int CATEGORIES_VISIBLE = 4;  // how many fit on screen at once
static bool _showCategories = false;       // toggled by "Browse" button

static bool isGutenbergServer() {
    if (_state.serverIndex < 0 || _state.serverIndex >= (int)_servers.size()) return false;
    return _servers[_state.serverIndex].isDefault &&
           _servers[_state.serverIndex].baseUrl.indexOf("gutenberg.org") >= 0;
}

static String gutenbergSearchUrl(const char* query) {
    return String("https://www.gutenberg.org/ebooks/search.opds/?query=") + query + "&sort_order=downloads";
}

void opds_store_init() {
    _servers = opds_load_servers();
    _state.scrollOffset = 0;
    _state.serverIndex = 0;
    _state.statusMsg = "";
    _state.downloading = false;
    _state.downloadPct = 0;
    _state.entries.clear();
    _state.navStack.clear();
    _state.currentUrl = "";
    _needsLibraryRefresh = false;
    _showCategories = false;
    _categoryScroll = 0;

    // Find first server with a URL
    for (int i = 0; i < (int)_servers.size(); i++) {
        if (_servers[i].baseUrl.length() > 0) {
            _state.serverIndex = i;
            break;
        }
    }
}

OpdsStoreState& opds_store_state() { return _state; }
bool opds_store_needs_library_refresh() { return _needsLibraryRefresh; }
void opds_store_clear_refresh_flag() { _needsLibraryRefresh = false; }

// Check if a book file already exists on device
static bool bookOnDevice(const String& title) {
    File dir = SD.open(BOOKS_DIR);
    if (!dir) return false;
    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            name.toLowerCase();
            if (name.endsWith(".epub")) {
                // Simple title match
                String nameNoExt = name.substring(0, name.lastIndexOf('.'));
                String lowerTitle = title;
                lowerTitle.toLowerCase();
                if (lowerTitle.indexOf(nameNoExt) >= 0 || nameNoExt.indexOf(lowerTitle) >= 0) {
                    entry.close();
                    dir.close();
                    return true;
                }
            }
        }
        entry.close();
    }
    dir.close();
    return false;
}

// Generate a safe filename from title
static String safeFilename(const String& title) {
    String name = title;
    name.trim();
    // Replace problematic chars
    for (int i = 0; i < (int)name.length(); i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            name.setCharAt(i, '_');
        }
    }
    // Truncate to reasonable length
    if (name.length() > 60) name = name.substring(0, 60);
    return name + ".epub";
}

// Fetch catalog for current server
static void fetchCurrentCatalog() {
    if (_state.serverIndex < 0 || _state.serverIndex >= (int)_servers.size()) {
        _state.statusMsg = "No server selected";
        return;
    }

    const OpdsServer& server = _servers[_state.serverIndex];
    if (server.baseUrl.length() == 0) {
        _state.statusMsg = "Server URL not set";
        _state.entries.clear();
        return;
    }

    String url = _state.currentUrl.length() > 0 ? _state.currentUrl : server.baseUrl;
    _state.statusMsg = "Loading...";
    _state.entries.clear();
    _state.scrollOffset = 0;

    // Connect WiFi if needed
    if (WiFi.status() != WL_CONNECTED) {
        const Settings& s = settings_get();
        WiFi.mode(WIFI_STA);
        WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            delay(250);
        }
        if (WiFi.status() != WL_CONNECTED) {
            _state.statusMsg = "WiFi failed";
            return;
        }
    }

    _state.entries = _client.fetchCatalog(url, server.username, server.password);

    if (_state.entries.empty()) {
        String err = _client.getLastError();
        _state.statusMsg = err.length() > 0 ? err : "No entries found";
    } else {
        _state.statusMsg = "";
    }
}

// ─── Drawing ────────────────────────────────────────────────────────

void opds_store_draw() {
    display_set_font_size(2);
    display_fill_screen(15);

    // Header
    display_draw_filled_rect(0, 0, W, STORE_HEADER_H, 2);
    display_draw_text(MARGIN, STORE_HEADER_H - 18, "Book Store", 15);

    // Browse button (Gutenberg only) and Back button in header
    int headerBtnX = W - MARGIN;
    const char* backBtn = "< Back";
    int bw = display_text_width(backBtn);
    headerBtnX -= bw;
    display_draw_text(headerBtnX, STORE_HEADER_H - 18, backBtn, 15);

    if (isGutenbergServer()) {
        const char* browseBtn = _showCategories ? "[Genres]" : "Genres";
        int brw = display_text_width(browseBtn);
        headerBtnX -= brw + 20;
        display_draw_text(headerBtnX, STORE_HEADER_H - 18, browseBtn, _showCategories ? 12 : 15);
    }

    display_draw_hline(0, STORE_HEADER_H, W, 0);

    // Server tabs
    int tabY = STORE_HEADER_H;
    display_draw_filled_rect(0, tabY, W, SERVER_TAB_H, 14);
    int tabX = MARGIN;
    for (int i = 0; i < (int)_servers.size(); i++) {
        String label = _servers[i].name;
        if (label.length() > 14) label = label.substring(0, 11) + "...";
        int tw = display_text_width(label.c_str());

        if (i == _state.serverIndex) {
            display_draw_filled_rect(tabX - 4, tabY + 4, tw + 8, SERVER_TAB_H - 8, 15);
            display_draw_text(tabX, tabY + SERVER_TAB_H - 12, label.c_str(), 0);
        } else {
            display_draw_text(tabX, tabY + SERVER_TAB_H - 12, label.c_str(), 6);
        }
        tabX += tw + 24;
    }
    display_draw_hline(0, tabY + SERVER_TAB_H, W, 10);

    int contentY = STORE_HEADER_H + SERVER_TAB_H;

    // Category row (Gutenberg genre picker)
    if (_showCategories && isGutenbergServer()) {
        int catY = contentY;
        display_draw_filled_rect(0, catY, W, CATEGORY_ROW_H, 13);

        // Draw visible categories
        int catX = MARGIN;
        int endIdx = min(_categoryScroll + CATEGORIES_VISIBLE, NUM_CATEGORIES);

        // Left arrow if scrollable
        if (_categoryScroll > 0) {
            display_draw_text(4, catY + CATEGORY_ROW_H - 10, "<", 3);
        }

        display_set_font_size(1);
        for (int i = _categoryScroll; i < endIdx; i++) {
            const char* label = GUTENBERG_CATEGORIES[i].label;
            int tw = display_text_width(label);
            display_draw_filled_rect(catX, catY + 6, tw + 12, CATEGORY_ROW_H - 12, 15);
            display_draw_rect(catX, catY + 6, tw + 12, CATEGORY_ROW_H - 12, 8);
            display_draw_text(catX + 6, catY + CATEGORY_ROW_H - 12, label, 0);
            catX += tw + 20;
        }
        display_set_font_size(2);

        // Right arrow if more
        if (endIdx < NUM_CATEGORIES) {
            display_draw_text(W - MARGIN + 4, catY + CATEGORY_ROW_H - 10, ">", 3);
        }

        display_draw_hline(0, catY + CATEGORY_ROW_H, W, 10);
        contentY += CATEGORY_ROW_H;
    }

    // Download progress overlay
    if (_state.downloading) {
        int cy = H / 2 - 40;
        const char* msg = "Downloading...";
        int mw = display_text_width(msg);
        display_draw_text((W - mw) / 2, cy, msg, 0);

        // Progress bar
        int barX = MARGIN + 20;
        int barW = W - barX * 2;
        int barY = cy + 30;
        int barH = 16;
        display_draw_rect(barX, barY, barW, barH, 0);
        int fillW = (barW - 4) * _state.downloadPct / 100;
        if (fillW > 0) {
            display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
        }

        char pctStr[16];
        snprintf(pctStr, sizeof(pctStr), "%d%%", _state.downloadPct);
        int pw = display_text_width(pctStr);
        display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

        display_update_fast();
        return;
    }

    // Status message
    if (_state.statusMsg.length() > 0) {
        int cy = H / 2 - 20;
        int sw = display_text_width(_state.statusMsg.c_str());
        display_draw_text((W - sw) / 2, cy, _state.statusMsg.c_str(), 4);

        if (_state.entries.empty()) {
            display_draw_hline(0, H - STORE_FOOTER_H, W, 8);
            display_update_fast();
            return;
        }
    }

    // Entry list
    int y = contentY + 6;
    int entriesPerPage = max(1, (H - contentY - STORE_FOOTER_H - 6) / ENTRY_CARD_H);
    int endIdx = min(_state.scrollOffset + entriesPerPage, (int)_state.entries.size());

    for (int i = _state.scrollOffset; i < endIdx; i++) {
        const OpdsEntry& e = _state.entries[i];

        // Title
        display_set_font_size(2);
        String title = e.title;
        int btnW = 0;

        if (e.isNavigation) {
            btnW = display_text_width(">") + 10;
        } else {
            btnW = display_text_width("[ GET ]") + 10;
        }

        int maxTitleW = W - MARGIN * 2 - btnW;
        while (title.length() > 3 && display_text_width(title.c_str()) > maxTitleW) {
            title = title.substring(0, title.length() - 4) + "...";
        }
        display_draw_text(MARGIN, y + FONT_H - 12, title.c_str(), 0);

        // Author line (smaller font)
        display_set_font_size(1);
        if (e.author.length() > 0) {
            String author = e.author;
            int maxAuthorW = W - MARGIN * 2 - btnW;
            while (author.length() > 3 && display_text_width(author.c_str()) > maxAuthorW) {
                author = author.substring(0, author.length() - 4) + "...";
            }
            display_draw_text(MARGIN, y + FONT_H + 18, author.c_str(), 6);
        }
        display_set_font_size(2);

        // Action button
        if (e.isNavigation) {
            display_draw_text(W - MARGIN - display_text_width(">"), y + FONT_H - 12, ">", 3);
        } else {
            bool onDevice = bookOnDevice(e.title);
            const char* btn = onDevice ? "ON DEV" : "[ GET ]";
            int btnTw = display_text_width(btn);
            display_draw_text(W - MARGIN - btnTw, y + FONT_H - 12, btn, onDevice ? 8 : 3);
        }

        display_draw_hline(MARGIN, y + ENTRY_CARD_H - 4, W - MARGIN * 2, 13);
        y += ENTRY_CARD_H;
    }

    // Footer with pagination
    int footerY = H - STORE_FOOTER_H;
    display_draw_filled_rect(0, footerY, W, STORE_FOOTER_H, 13);
    display_draw_hline(0, footerY, W, 8);

    if (!_state.entries.empty()) {
        // Nav stack back
        if (!_state.navStack.empty()) {
            display_draw_text(MARGIN, footerY + STORE_FOOTER_H - 12, "< Back", 3);
        }

        // Page info
        int curPage = _state.scrollOffset / max(1, entriesPerPage) + 1;
        int totalPg = ((int)_state.entries.size() + entriesPerPage - 1) / entriesPerPage;
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d / %d", curPage, totalPg);
        int piw = display_text_width(pageInfo);
        display_draw_text(W / 2 - piw / 2, footerY + STORE_FOOTER_H - 12, pageInfo, 6);

        // Next page link
        String nextUrl = _client.getNextPageUrl();
        bool hasMorePages = (_state.scrollOffset + entriesPerPage < (int)_state.entries.size());
        if (hasMorePages || nextUrl.length() > 0) {
            const char* next = "Next >";
            int nw = display_text_width(next);
            display_draw_text(W - MARGIN - nw, footerY + STORE_FOOTER_H - 12, next, 3);
        }
    }

    display_update_fast();
}

// ─── Touch handling ─────────────────────────────────────────────────

void opds_store_handle_touch(int x, int y) {
    // If downloading, ignore touches
    if (_state.downloading) return;

    // Header: Back button (right side) and Genres button (Gutenberg)
    if (y < STORE_HEADER_H) {
        // Back button is always rightmost
        const char* backBtn = "< Back";
        int bw = display_text_width(backBtn);
        if (x > W - MARGIN - bw - 10) {
            _state.statusMsg = "__BACK__";
            return;
        }
        // Genres toggle (Gutenberg only)
        if (isGutenbergServer() && x > W / 3) {
            _showCategories = !_showCategories;
            _categoryScroll = 0;
            return;  // redraw will show/hide category row
        }
        return;
    }

    // Server tabs
    int tabY = STORE_HEADER_H;
    if (y >= tabY && y < tabY + SERVER_TAB_H) {
        int tabX = MARGIN;
        for (int i = 0; i < (int)_servers.size(); i++) {
            String label = _servers[i].name;
            if (label.length() > 14) label = label.substring(0, 11) + "...";
            int tw = display_text_width(label.c_str());
            if (x >= tabX - 4 && x < tabX + tw + 20) {
                if (i != _state.serverIndex) {
                    _state.serverIndex = i;
                    _state.currentUrl = "";
                    _state.navStack.clear();
                    _showCategories = false;
                    fetchCurrentCatalog();
                }
                return;
            }
            tabX += tw + 24;
        }
        return;
    }

    // Category row touch handling
    int contentY = STORE_HEADER_H + SERVER_TAB_H;
    if (_showCategories && isGutenbergServer()) {
        if (y >= contentY && y < contentY + CATEGORY_ROW_H) {
            // Left/right scroll arrows
            if (x < MARGIN && _categoryScroll > 0) {
                _categoryScroll = max(0, _categoryScroll - CATEGORIES_VISIBLE);
                return;
            }
            if (x > W - MARGIN && _categoryScroll + CATEGORIES_VISIBLE < NUM_CATEGORIES) {
                _categoryScroll += CATEGORIES_VISIBLE;
                if (_categoryScroll >= NUM_CATEGORIES) _categoryScroll = max(0, NUM_CATEGORIES - CATEGORIES_VISIBLE);
                return;
            }

            // Category button taps
            display_set_font_size(1);
            int catX = MARGIN;
            int endCat = min(_categoryScroll + CATEGORIES_VISIBLE, NUM_CATEGORIES);
            for (int i = _categoryScroll; i < endCat; i++) {
                const char* label = GUTENBERG_CATEGORIES[i].label;
                int tw = display_text_width(label);
                if (x >= catX && x < catX + tw + 12) {
                    // Navigate to this genre search
                    _state.navStack.push_back(_state.currentUrl);
                    _state.currentUrl = gutenbergSearchUrl(GUTENBERG_CATEGORIES[i].query);
                    display_set_font_size(2);
                    fetchCurrentCatalog();
                    return;
                }
                catX += tw + 20;
            }
            display_set_font_size(2);
            return;
        }
        contentY += CATEGORY_ROW_H;
    }

    // Compute dynamic entries per page (matches draw code)
    int entriesPerPage = max(1, (H - contentY - STORE_FOOTER_H - 6) / ENTRY_CARD_H);

    // Footer
    int footerY = H - STORE_FOOTER_H;
    if (y >= footerY) {
        if (x < W / 3 && !_state.navStack.empty()) {
            // Navigate back
            _state.currentUrl = _state.navStack.back();
            _state.navStack.pop_back();
            fetchCurrentCatalog();
            return;
        }
        if (x > W * 2 / 3) {
            // Next page
            if (_state.scrollOffset + entriesPerPage < (int)_state.entries.size()) {
                _state.scrollOffset += entriesPerPage;
            } else {
                // Try OPDS next page
                String nextUrl = _client.getNextPageUrl();
                if (nextUrl.length() > 0) {
                    _state.currentUrl = nextUrl;
                    fetchCurrentCatalog();
                }
            }
            return;
        }
        if (x < W / 3 && _state.scrollOffset > 0) {
            _state.scrollOffset -= entriesPerPage;
            if (_state.scrollOffset < 0) _state.scrollOffset = 0;
            return;
        }
        return;
    }

    // Entry taps
    contentY += 6;
    if (y > contentY) {
        int entryIdx = _state.scrollOffset + (y - contentY) / ENTRY_CARD_H;
        if (entryIdx >= 0 && entryIdx < (int)_state.entries.size()) {
            const OpdsEntry& e = _state.entries[entryIdx];

            if (e.isNavigation) {
                // Navigate into subcatalog
                _state.navStack.push_back(_state.currentUrl);
                _state.currentUrl = e.navUrl;
                fetchCurrentCatalog();
            } else if (e.acquisitionUrl.length() > 0) {
                // Download EPUB
                String filename = safeFilename(e.title);
                String targetPath = String(BOOKS_DIR) + "/" + filename;

                // Check if already on device
                if (SD.exists(targetPath)) {
                    _state.statusMsg = "Already on device";
                    return;
                }

                _state.downloading = true;
                _state.downloadPct = 0;

                // Draw initial download screen
                opds_store_draw();

                const OpdsServer& server = _servers[_state.serverIndex];
                bool ok = _client.downloadEpub(
                    e.acquisitionUrl, targetPath,
                    server.username, server.password,
                    [](int pct) {
                        _state.downloadPct = pct;
                        if (pct % 10 == 0) {
                            // Redraw progress
                            opds_store_draw();
                        }
                    }
                );

                _state.downloading = false;
                if (ok) {
                    _state.statusMsg = "Downloaded: " + e.title;
                    _needsLibraryRefresh = true;
                } else {
                    _state.statusMsg = "Error: " + _client.getLastError();
                }
            }
        }
    }
}
