# T5 E-Reader Firmware — Implementation Plan

**Date:** 2026-03-31
**Author:** [architect] agent (Opus)
**Status:** Phase 1 COMPLETE — implemented 2026-03-31, branch `phase1-ux-performance`

---

## Executive Summary

The current firmware is a working but minimal e-reader (~2060 lines of C++ across 9 source modules). It successfully reads EPUBs, renders text, handles touch input, and saves progress. However, it lacks essential usability features (settings, TOC, bookmarks, home navigation) and has significant performance bottlenecks (full-screen redraws, no metadata caching, pixel-by-pixel rotation).

This plan defines **Phase 1** (UX foundations + core performance groundwork) in enough detail for Claude Code to implement without guesswork, plus a documented roadmap for later phases.

---

## Architecture Analysis — Current State

### Module Map (current)
```
main.cpp (568 lines)   — State machine, ALL UI drawing, touch handlers, sleep
config.h (70 lines)    — Pin defs, WiFi creds, layout constants
display.cpp (212 lines) — EPD wrapper, portrait framebuffer, pixel-by-pixel rotation
epub.cpp (585 lines)   — ZIP reader, EPUB parser, HTML stripper, entity decoder
reader.cpp (268 lines) — Text wrapping, pagination, progress save/load
library.cpp (144 lines) — SD card scanning, progress loading
touch.cpp (86 lines)   — GT911 driver, coordinate transforms
battery.cpp (25 lines) — ADC voltage, LiPo % estimate
wifi_upload.cpp (171 lines) — WiFi AP, WebServer, file upload
```

### Key Performance Bottlenecks Identified

| Issue | Impact | Location |
|-------|--------|----------|
| **Library scan opens every EPUB** to extract title | Multi-second startup for >5 books | `library.cpp:scanDir()` |
| **display_update() always does epd_clear() + full redraw** | Visible full-white flash on every page turn | `display.cpp:display_update()` |
| **Pixel-by-pixel rotation** (518,400 iterations with branches) | ~100ms+ per frame on ESP32-S3 | `display.cpp:display_update()` |
| **Full chapter re-parse + re-wrap** on every chapter switch | Noticeable lag on chapter boundaries | `reader.cpp:loadChapter()` |
| **No pre-computation or caching** of any kind | Every operation is from-scratch | Throughout |
| **Arduino String everywhere** | Heap fragmentation over time | Throughout |

### UX Gaps

- **No settings page** — font size, sleep timeout, WiFi creds all hardcoded
- **No obvious way home** — center-tap menu exists but is discoverable only by accident; only option is "Back to Library"
- **No TOC / chapter picker** — linear reading only; no way to jump to a chapter
- **No bookmarks** — progress save is automatic but user can't mark/return to specific locations
- **"Continue Reading" picks first book with progress** — no timestamp, no "most recent" logic

---

## Phase 1 — UX Foundations + Core Performance Groundwork

### Scope
Everything below is in-scope for Phase 1. This is designed to be implementable in one focused sprint (est. 4–8 hours of Claude Code time).

---

### 1.1 — New Module: `settings.cpp/.h` (Settings & Preferences)

**Purpose:** Persistent user preferences stored as JSON on SD card.

**File:** `src/settings.h`
```cpp
#pragma once
#include <Arduino.h>

struct Settings {
    // Display
    int   fontSize;         // 0=small, 1=medium(default), 2=large
    int   sleepTimeoutMin;  // minutes before deep sleep (default: 5)
    
    // WiFi
    String wifiSSID;
    String wifiPass;
    
    // Reading
    bool   showPageNumbers;  // footer page numbers (default: true)
    bool   showBattery;      // battery in header (default: true)
    int    tapZoneLayout;    // 0=left/center/right (default), 1=top/mid/bottom
};

void settings_init();           // Load from SD or create defaults
void settings_save();           // Write to SD as JSON
Settings& settings_get();       // Reference to live settings
void settings_set_default();    // Reset to factory defaults
```

**Storage:** `/books/.settings.json` on SD card (alongside `.progress/`).

**Implementation notes:**
- Use ArduinoJson (already a dependency) for serialization
- Load in `setup()` after `library_init()` but before UI draws
- `settings_save()` writes atomically: write to `.settings.tmp`, then rename
- Font size maps to a concrete `_linesPerPage` / `_maxLineWidth` adjustment (details in §1.6)
- WiFi creds from settings replace the hardcoded `WIFI_SSID` / `WIFI_PASS` defines at runtime

---

### 1.2 — New Screen: Settings UI

**New app state:** `STATE_SETTINGS`

**Layout (portrait 540×960):**
```
┌─────────────────────────────┐
│ ■ Settings            [bat]  │  ← dark header
├─────────────────────────────┤
│                              │
│  Font Size        [< Med >]  │  ← tap left/right arrows to cycle
│  ─────────────────────────── │
│  Sleep Timeout    [< 5m  >]  │  ← cycle: 2, 5, 10, 15, 30 min
│  ─────────────────────────── │
│  WiFi SSID        crazy      │  ← display only (edit via web UI)
│  ─────────────────────────── │
│  Page Numbers     [  ON  ]   │  ← toggle
│  ─────────────────────────── │
│  Battery Display  [  ON  ]   │  ← toggle
│  ─────────────────────────── │
│                              │
│  [ Reset Defaults ]          │
│                              │
├─────────────────────────────┤
│        [ Back to Library ]   │  ← footer
└─────────────────────────────┘
```

**Touch handling:** Each settings row is a horizontal touch zone. Left/right half of the value area cycles the option. Bottom bar returns to library.

**Implementation:** Add `drawSettingsScreen()` and `handleSettingsTouch()` in `main.cpp` (following the existing pattern for other screens).

---

### 1.3 — New Screen: Table of Contents (TOC / Chapter Picker)

**New app state:** `STATE_TOC`

**Data source:** The EPUB spine is already parsed (`_spine` vector in `EpubParser`). We need to expose chapter names.

**Required changes to `epub.h/.cpp`:**
- Add `String getChapterTitle(int index)` to `EpubParser` — extracts the `<title>` tag from the chapter XHTML, or falls back to the spine `id` field
- Add `const std::vector<SpineItem>& getSpine() const` accessor

**Layout:**
```
┌─────────────────────────────┐
│ ■ Table of Contents   [bat]  │
├─────────────────────────────┤
│                              │
│  ► Chapter 1: Title...       │  ← current chapter highlighted
│  ─────────────────────────── │
│    Chapter 2: Title...       │
│  ─────────────────────────── │
│    Chapter 3: Title...       │
│    ...                       │
│                              │
│         1-8 of 24    ▼       │  ← scroll indicator
├─────────────────────────────┤
│        [ Back to Reading ]   │
└─────────────────────────────┘
```

**Touch handling:**
- Tap a chapter → jump reader to that chapter, page 0, return to `STATE_READER`
- Swipe/tap on scroll indicator area → scroll list
- Footer → back to reader

**Integration with reader:** Add `void jumpToChapter(int chapter)` to `BookReader` (calls `loadChapter(chapter)` with `_currentPage = 0`).

---

### 1.4 — New Feature: Bookmarks

**Storage:** Extend the existing per-book `.json` progress file.

**New fields in progress JSON:**
```json
{
  "chapter": 5,
  "page": 3,
  "total_chapters": 24,
  "last_read": 1711872000,
  "bookmarks": [
    {"chapter": 2, "page": 7, "label": "Bookmark 1"},
    {"chapter": 5, "page": 0, "label": "Bookmark 2"}
  ]
}
```

**New `BookReader` methods:**
```cpp
void addBookmark();           // Bookmark current position
void removeBookmark(int idx); // Remove by index
const std::vector<Bookmark>& getBookmarks() const;
bool jumpToBookmark(int idx);
```

**UI integration:**
- **Add bookmark:** Long-press center zone in reader (>800ms hold) — adds current chapter+page
- **View bookmarks:** New menu item in the reader overlay menu (STATE_MENU)
- **Bookmark indicator:** Small dot or "★" in reader header when current page is bookmarked
- **Bookmark list:** Similar to TOC layout — scrollable list, tap to jump

**New app state:** `STATE_BOOKMARKS` (accessible from reader menu)

---

### 1.5 — Navigation Overhaul (Home Button + Menu Redesign)

**Problem:** The only way to access the menu is a center-tap that's not discoverable. The menu itself is minimal (just "Back to Library" + progress info).

**Solution — Redesigned Reader Menu (STATE_MENU):**
```
┌─────────────────────────────┐
│                              │
│     "Book Title Here"        │
│     Chapter 5 of 24          │
│     Page 3 of 12             │
│  ─────────────────────────── │
│                              │
│     📖  Table of Contents    │  ← tap → STATE_TOC
│  ─────────────────────────── │
│     🔖  Bookmarks (3)        │  ← tap → STATE_BOOKMARKS
│  ─────────────────────────── │
│     ⚙   Settings             │  ← tap → STATE_SETTINGS
│  ─────────────────────────── │
│     🏠  Library               │  ← tap → STATE_LIBRARY (saves progress)
│  ─────────────────────────── │
│                              │
│    Tap anywhere to resume    │
│                              │
└─────────────────────────────┘
```

**Touch zones:** Each menu item is a row. Tap outside any item → back to reader.

**Library screen footer redesign:**
```
Current:  [ WiFi Upload ]
New:      [ WiFi Upload ]  |  [ ⚙ Settings ]
```
Split the footer into two touch zones.

---

### 1.6 — Core Performance Groundwork

These are the performance improvements that lay the foundation for a responsive UX. They don't add features but make existing features usably fast.

#### 1.6.1 — Library Metadata Cache

**Problem:** `library_scan()` opens every EPUB file (ZIP parse + OPF parse) to extract the title. For 10+ books this takes several seconds.

**Solution:** Cache metadata to SD card.

**File:** `/books/.library_cache.json`
```json
[
  {"path": "/books/Enders Game.epub", "size": 1234567, "title": "Ender's Game", "chapters": 18},
  {"path": "/books/Odd Thomas.epub", "size": 2345678, "title": "The Odd Thomas Series", "chapters": 408}
]
```

**Logic in `library_scan()`:**
1. Read cache file
2. For each `.epub` on disk, check if cache entry exists with matching path + file size
3. If cache hit → use cached title/chapters
4. If cache miss → open EPUB, extract metadata, add to cache
5. Write updated cache back to disk
6. Remove stale entries (files no longer on disk)

**Expected speedup:** First scan same as before; subsequent scans skip EPUB parsing entirely (~10x faster for >5 books).

#### 1.6.2 — Optimized Display Rotation

**Problem:** `display_update()` iterates 518,400 pixels with per-pixel index calculation and branching.

**Solution:** Optimize the rotation loop with direct byte-level operations.

**Approach:**
```cpp
// Instead of per-pixel pget/pset through the rotation:
// Process 2 portrait pixels at a time (one byte = 2 pixels in 4bpp)
// Pre-compute landscape byte positions
// Use a lookup table for the nibble swap patterns
```

**Concrete implementation:**
- Process portrait framebuffer in column-major order (iterate px outer, py inner)
- Each column of portrait pixels maps to a contiguous row in landscape buffer
- Write landscape bytes directly without per-pixel branching
- Target: <50ms rotation (down from ~100-150ms)

#### 1.6.3 — Partial Display Updates (Preparation)

**Problem:** Every page turn does `epd_clear()` (full white flash) then full redraw. This is visually jarring and slow.

**Solution:** Use epdiy's partial update capability.

**Changes to `display.cpp`:**
```cpp
// New function: update only the changed region
void display_update_partial();

// New function: update with mode selection
void display_update_mode(bool fullRefresh);
```

**Strategy:**
- Page turns → partial update (no white flash, ~300ms instead of ~800ms)
- Every Nth page turn (e.g., every 10) → full refresh to prevent ghosting
- Screen transitions (library→reader, menu, etc.) → full refresh
- Add `_fullRefreshCounter` to track when a full refresh is needed

**epdiy API used:**
```cpp
// Partial: draw over existing content without clearing
epd_poweron();
epd_draw_grayscale_image(epd_full_screen(), _lfb);  // no epd_clear()
epd_poweroff();
```

**Note:** epdiy's `epd_draw_grayscale_image()` without prior `epd_clear()` acts as a partial update on ED047TC1. The ghosting is manageable for text and can be periodically cleaned with a full refresh.

#### 1.6.4 — Chapter Pre-caching

**Problem:** Switching chapters re-parses HTML and re-wraps all text, causing a noticeable pause.

**Solution:** Pre-cache adjacent chapters in the background.

**Changes to `reader.cpp`:**
```cpp
// Cache structure: chapter index → wrapped lines
struct ChapterCache {
    int chapter;
    std::vector<String> wrappedLines;
    std::vector<PageRange> pages;
};

// Keep current + next + previous chapter cached
ChapterCache _cache[3];  // [prev, current, next]
```

**Background loading:**
- After rendering current page, if `loop()` has idle time (no touch), pre-load next chapter
- When advancing to next chapter, the data is already cached → instant transition
- Evict the "previous previous" chapter when moving forward

**Memory budget:** Each chapter's wrapped lines ≈ 10-50KB. 3 chapters ≈ 30-150KB. ESP32-S3 has 8MB PSRAM — this is trivial.

#### 1.6.5 — "Last Read" Timestamp for Continue Reading

**Problem:** `library_find_current_book()` returns the first book with progress, not the most recently read.

**Fix:** Add `"last_read": <unix_timestamp>` to progress JSON. `library_find_current_book()` returns the book with the highest timestamp.

**Implementation:** Use `millis()` / boot count as a proxy, or use the RTC (PCF8563 is on the I2C bus per `config.h`). Simplest approach: use a monotonic counter stored in NVS that increments on each book open.

---

### 1.7 — Font Size Implementation

**Problem:** Settings §1.2 adds a font size control, but the current code uses a single hardcoded font.

**Practical approach (no new font files needed):**

The LilyGo-EPD47 library includes `FiraSans` at one size. Rather than loading multiple font files (which would require significant flash space), implement font size via **line spacing and margins**:

| Size | Lines/Page | Line Spacing | Side Margins |
|------|-----------|-------------|-------------|
| Small | +4 lines | LINE_SPACING=2 | MARGIN_X=20 |
| Medium (default) | current | LINE_SPACING=4 | MARGIN_X=30 |
| Large | -4 lines | LINE_SPACING=8 | MARGIN_X=40 |

This changes the effective text density without needing a new font. The font rendering is identical, but the "large" setting gives more whitespace per line (easier on the eyes) and fewer lines per page.

**Future Phase 2 enhancement:** Bundle 2-3 font sizes (requires adding font resources to flash).

---

## Proposed Architecture — After Phase 1

### New Module Map
```
main.cpp          — State machine, touch routing, screen dispatch (slimmed)
config.h          — Pin defs, layout constants (WiFi creds removed)
settings.cpp/.h   — NEW: Persistent preferences, JSON on SD
display.cpp/.h    — EPD wrapper + partial update + optimized rotation
epub.cpp/.h       — ZIP/EPUB parser + chapter title extraction
reader.cpp/.h     — Pagination, bookmarks, chapter cache, progress
library.cpp/.h    — SD scanning + metadata cache + last-read ordering
touch.cpp/.h      — GT911 driver (unchanged)
battery.cpp/.h    — ADC monitoring (unchanged)
wifi_upload.cpp/.h — WiFi upload (reads creds from settings)
```

### New App States
```cpp
enum AppState {
    STATE_BOOT,
    STATE_LIBRARY,
    STATE_READER,
    STATE_MENU,         // redesigned reader overlay
    STATE_TOC,          // NEW: table of contents
    STATE_BOOKMARKS,    // NEW: bookmark list
    STATE_SETTINGS,     // NEW: settings page
    STATE_WIFI
};
```

### Data Flow Changes
```
setup() → settings_init() → library_init() → library_scan(cached) → draw
                                                      ↑
                                             metadata cache on SD
reader.openBook() → loadChapter() → pre-cache adjacent chapters
                                         ↑
                                    chapter cache in PSRAM
display_update() → optimized rotation → partial/full update selection
```

---

## Milestones & Acceptance Criteria

### Milestone 1: Settings Infrastructure (est. 45 min) — COMPLETE
- [x] `settings.cpp/.h` created with load/save/defaults
- [x] Settings JSON created on first boot, persists across reboots
- [x] `wifi_upload.cpp` reads WiFi creds from settings instead of defines
- [x] Sleep timeout configurable via settings
- **Accept:** Settings survive power cycle; WiFi connects using stored creds

### Milestone 2: Library Metadata Cache (est. 30 min) — COMPLETE
- [x] `.library_cache.json` created on first scan
- [x] Subsequent scans skip EPUB parsing for cached books
- [x] New books are detected and cached
- [x] Deleted books are pruned from cache
- **Accept:** Second library scan completes in <1 second for 10 books

### Milestone 3: Display Performance (est. 60 min) — COMPLETE
- [x] Rotation loop optimized (measurable with `millis()` before/after)
- [x] Partial update mode implemented
- [x] Page turns use partial update; every 10th turn does full refresh
- [x] Screen transitions (state changes) use full refresh
- **Accept:** Page turn latency reduced by >30%; no white flash on normal turns

### Milestone 4: Chapter Pre-cache (est. 30 min) — COMPLETE
- [x] 3-chapter cache structure in reader
- [x] Next chapter pre-loaded during idle time in `loop()`
- [x] Chapter transitions use cached data when available
- **Accept:** Forward chapter transition is instantaneous when pre-cached

### Milestone 5: Navigation + Menu Redesign (est. 45 min) — COMPLETE
- [x] Reader menu shows TOC, Bookmarks, Settings, Library options
- [x] Settings screen drawn and interactive
- [x] Settings accessible from both reader menu and library footer
- **Accept:** All menu items navigate correctly; settings changes persist

### Milestone 6: TOC Screen (est. 45 min) — COMPLETE
- [x] `EpubParser::getChapterTitle()` implemented
- [x] TOC screen shows scrollable chapter list
- [x] Current chapter highlighted
- [x] Tap chapter → jump to it and return to reader
- **Accept:** Can navigate from Ch 1 to Ch 15 via TOC without reading linearly

### Milestone 7: Bookmarks (est. 45 min) — COMPLETE
- [x] Bookmark data stored in progress JSON
- [x] Long-press center zone adds bookmark
- [x] Bookmark list screen with jump-to functionality
- [x] Bookmark indicator in reader header
- **Accept:** Can add 3 bookmarks, view them in list, jump to each

### Milestone 8: Last-Read Ordering + Font Size (est. 30 min) — COMPLETE
- [x] Progress JSON includes monotonic counter or timestamp
- [x] "Continue Reading" shows most recently read book
- [x] Font size setting changes line density
- [x] Reader re-paginates when font size changes
- **Accept:** After reading Book B, "Continue Reading" shows Book B (not Book A)

---

## Risk List & Fallback Strategy

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|------------|
| 1 | **Partial display update causes unacceptable ghosting** | Medium | Medium | Fall back to full refresh with reduced `epd_clear()` duration; or use `epd_clear_area()` for text region only |
| 2 | **Chapter title extraction fails** (no `<title>` in XHTML) | High | Low | Fall back to spine ID or "Chapter N" label; extract from `<h1>`/`<h2>` as second attempt |
| 3 | **PSRAM allocation for chapter cache fails** | Low | Medium | Reduce cache to 2 chapters (current + next only); use `ps_malloc` with null checks |
| 4 | **Settings JSON corruption on power loss** | Medium | Low | Atomic write (write .tmp + rename); on parse failure, reset to defaults |
| 5 | **Long-press detection interferes with tap** | Medium | Medium | Use 800ms threshold; if problematic, change to double-tap instead |
| 6 | **Library cache invalidation** (book replaced with same-size file) | Low | Low | Include file modification time if available from SD FAT; or add CRC of first 1KB |
| 7 | **Font size change breaks text wrapping layout** | Low | Medium | Re-wrap and re-paginate on font size change; test with all 3 sizes |
| 8 | **Stack overflow from recursive chapter loading** | Low | High | Pre-cache runs in `loop()` idle, not nested; keep call depth shallow |

---

## Roadmap — Later Phases

### Phase 2: Rich Reading Experience (Priority: High)
- **Real font size support** — Bundle FiraSans at 2-3 sizes (20pt, 26pt, 32pt); switch font pointer in display module
- **Image rendering** — Decode JPEG/PNG from EPUB, dither to 4-bit grayscale, render inline
- **Brightness/contrast** — epdiy supports different waveform intensities; expose as setting
- **Reading statistics** — Track time-per-chapter, pages-per-session, store in progress JSON
- **Search within book** — Full-text search across loaded chapter text

### Phase 3: Connectivity & Sync (Priority: Medium)
- **WiFi settings via web UI** — Add settings form to the upload page (no e-ink keyboard needed)
- **OTA firmware updates** — Use ESP32 OTA partition (already in partition table: `ota_0`)
- **Book deletion** — Delete books from the web UI or from device
- **USB Mass Storage** — ESP32-S3 USB OTG as mass storage for drag-and-drop book loading
- **Time sync** — NTP when WiFi connected; use RTC (PCF8563) for timestamps

### Phase 4: Polish & Advanced Features (Priority: Low)
- **Gesture support** — Swipe left/right for page turns (instead of tap zones only)
- **Dictionary lookup** — Long-press on word (would need a dictionary file on SD)
- **Night mode** — Inverted colors (white text on black) for e-ink
- **Collections/shelves** — Organize books into categories
- **OPDS catalog** — Browse and download books over WiFi from Calibre/OPDS servers
- **Multi-language** — UI strings externalized for localization

---

## Implementation Strategy — Branch & Commit Approach

### Recommendation: **Single feature branch with checkpoint commits**

**Rationale:**
1. The codebase is small (~2060 lines). All changes interact closely (settings affects reader, library, display, WiFi).
2. Creating separate branches for each milestone would create merge conflicts (e.g., both TOC and Bookmarks modify main.cpp's state machine).
3. However, one giant commit is risky — if something breaks, hard to bisect.

**Approach:**
```
git checkout -b phase1-ux-performance

# Commit 1: Settings infrastructure (settings.cpp/.h, config.h cleanup)
# Commit 2: Library metadata cache
# Commit 3: Display optimizations (rotation + partial update)
# Commit 4: Chapter pre-cache
# Commit 5: Navigation redesign + menu overhaul (main.cpp state machine changes)
# Commit 6: TOC screen + chapter title extraction
# Commit 7: Bookmarks
# Commit 8: Font size + last-read ordering + final integration

git checkout main && git merge phase1-ux-performance
```

**Build verification:** After each commit, the firmware should compile cleanly (`pio run`). Not every intermediate commit needs to be functionally complete, but it must compile.

**Testing:** After the final commit, flash to device and verify each acceptance criterion. Serial monitor output should confirm each feature working.

---

## File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `src/settings.cpp` | **CREATE** | Preferences load/save/defaults |
| `src/settings.h` | **CREATE** | Settings struct + API |
| `src/main.cpp` | **MODIFY** | Add states, new screens, redesigned menu, settings entry point |
| `src/config.h` | **MODIFY** | Remove hardcoded WiFi creds; add new layout constants for font sizes |
| `src/display.cpp` | **MODIFY** | Optimized rotation, partial update support |
| `src/display.h` | **MODIFY** | Add `display_update_partial()`, `display_update_mode()` |
| `src/epub.cpp` | **MODIFY** | Add `getChapterTitle()` implementation |
| `src/epub.h` | **MODIFY** | Add `getChapterTitle()`, `getSpine()` accessor |
| `src/reader.cpp` | **MODIFY** | Chapter cache, bookmarks, `jumpToChapter()`, last-read counter |
| `src/reader.h` | **MODIFY** | Bookmark struct, cache struct, new methods |
| `src/library.cpp` | **MODIFY** | Metadata cache, last-read ordering |
| `src/library.h` | **MODIFY** | Update `BookInfo` with `lastReadOrder` field |
| `src/wifi_upload.cpp` | **MODIFY** | Read WiFi creds from settings |
| `src/touch.cpp` | **MODIFY** | Add long-press detection support |
| `src/touch.h` | **MODIFY** | Add `touch_read_long_press()` or duration field |

**New files:** 2 (settings.cpp, settings.h)
**Modified files:** 12
**Deleted files:** 0
**Estimated total new/changed lines:** ~800-1000

---

## Notes for Claude Code

1. **Always compile after each logical change** — run `pio run` (no upload needed for validation)
2. **The device is connected at `/dev/ttyACM0`** — upload with `pio run -t upload --upload-port /dev/ttyACM0` if testing is desired
3. **PSRAM is available** — use `ps_malloc()` / `ps_calloc()` for large allocations (8MB OPI PSRAM)
4. **ArduinoJson v6 is already a dependency** — use `StaticJsonDocument` for small docs, `DynamicJsonDocument` for larger ones
5. **The font `FiraSans` from LilyGo-EPD47** is the only available font; `get_glyph()` is the API for glyph access
6. **Touch coordinate system:** Landscape from GT911 → portrait transform in `touch.cpp` — don't double-transform
7. **epdiy library API:** `epd_init()`, `epd_poweron()`, `epd_clear()`, `epd_draw_grayscale_image()`, `epd_poweroff()`, `epd_full_screen()` — these are the available functions
8. **WiFi creds currently hardcoded** as `#define` in config.h — Phase 1 moves them to settings but should keep defines as fallback defaults
9. **Serial debug output** is useful — keep `Serial.printf()` logging for key operations
10. **The partition table** has 8MB for app, 7.9MB for SPIFFS (unused) — plenty of flash headroom
