#pragma once

#include <Arduino.h>
#include <vector>
#include "state.h"

// Forward declarations
class BookReader;

// Reader refresh state — passed by reference so the UI module can update
// refresh behavior without owning the state.
struct ReaderRefreshState {
    bool fastRefresh;
    bool chapterJump;
    bool forceFullRefresh;
    int  pageTurnsSinceFull;
};

// ─── Reader Screen ──────────────────────────────────────────────────

// Draw the reader screen (book text, header, progress bar, footer).
// Handles display update mode selection based on refresh state.
void ui_reader_draw(BookReader& reader, ReaderRefreshState& refresh);

// Handle touch on reader screen.
// Returns the new AppState (STATE_READER if staying, STATE_MENU if center tap).
// Updates refresh state and triggers bookmark add on long-press.
AppState ui_reader_touch(int x, int y, bool isLongPress,
                         BookReader& reader, ReaderRefreshState& refresh);

// ─── Menu Overlay ───────────────────────────────────────────────────

// Draw the reader menu overlay (TOC, Bookmarks, Settings, Library).
void ui_reader_menu_draw(BookReader& reader);

// Handle touch on menu overlay.
// Returns new AppState (STATE_READER, STATE_TOC, STATE_BOOKMARKS, etc.).
// tocScroll and bmScroll are reset when entering those screens.
AppState ui_reader_menu_touch(int x, int y, BookReader& reader,
                              ReaderRefreshState& refresh);

// ─── Go To ──────────────────────────────────────────────────────────

void ui_reader_goto_draw(BookReader& reader);
AppState ui_reader_goto_touch(int x, int y, BookReader& reader,
                              ReaderRefreshState& refresh);

// ─── Table of Contents ──────────────────────────────────────────────

// Draw the TOC screen.
void ui_reader_toc_draw(BookReader& reader, int& tocScroll);

// Handle touch on TOC screen.
// Returns STATE_READER on chapter selection or back, current state otherwise.
AppState ui_reader_toc_touch(int x, int y, BookReader& reader,
                             int& tocScroll, ReaderRefreshState& refresh);

// ─── Bookmarks ──────────────────────────────────────────────────────

// Draw the bookmarks screen.
void ui_reader_bookmarks_draw(BookReader& reader, int& bmScroll);

// Handle touch on bookmarks screen.
// Returns STATE_READER on selection or back, current state otherwise.
AppState ui_reader_bookmarks_touch(int x, int y, BookReader& reader,
                                   int& bmScroll, ReaderRefreshState& refresh);
