# T5 E-Reader — Phase 2 Design Plan

**Date:** 2026-03-31
**Hardware:** LilyGo T5 4.7" V2.4 (ESP32-S3, 16MB flash, 8MB OPI PSRAM)
**Branch:** `phase1-ux-performance` → merge into `master` before starting Phase 2

---

## 0. Phase 1 Status (complete as of this document)

| Area | Status |
|------|--------|
| Build toolchain | ✅ Clean |
| Touch input | ✅ GT911 working |
| EPUB ZIP/HTML parsing | ✅ VFS path fix applied |
| Library scan + covers | ✅ Metadata cache, poster view |
| Reader: paging, bookmarks | ✅ Working |
| TOC screen UI | ✅ Drawn, scrollable |
| TOC chapter selection | ⚠️ Fixed (Phase 1 bug — see below) |
| Sleep images | ⚠️ Fixed (Phase 1 bug — see below) |
| WiFi OTA upload | ✅ Working |
| Settings screen | ✅ Working |

### Phase 1 bugs fixed in this session

**TOC/chapter selection wedge** (`reader.cpp`, `main.cpp`)
- Root cause 1: `wrapText` yielded every 40 paragraphs. On dense chapters with many
  words per paragraph, 40 iterations could exceed the ESP32 TWDT timeout (default 5 s),
  causing a watchdog reset mid-EPD-clear → blank screen that appeared to be a permanent
  freeze. Fix: replaced per-paragraph counter with `millis()`-based yield every 50 ms.
- Root cause 2: No user feedback during the 2–5 s chapter load + EPD clear sequence.
  Fix: `handleTocTouch` now draws a "Loading chapter..." screen with `display_update_fast()`
  before calling `jumpToChapter`, so the device is visibly responsive.

**Sleep images not rendering** (`sleep_image.cpp`)
- Root cause: `pngDrawCallback` had `if (pDraw->iWidth > 1024) return 0` which silently
  aborted the decode for any PNG wider than 1024 px (virtually all real photos).
  Fix: removed the static `lineBuffer[1024]`; the context struct now carries a
  `ps_malloc`'d buffer sized to the actual image width, allocated before decode and
  freed after.

---

## 1. Reader Navigation Shell / Home Affordance

**Problem:** There is no "Home" gesture or button from within the reader. The only escape
is the three-zone tap → menu overlay. New users don't discover this easily, and the menu
overlay itself lacks a clear visual hierarchy.

**Proposed design:**

1. **Long-press top edge** (top 60 px of portrait screen) → jump to Library. This is a
   spatially intuitive gesture (the header bar is already a visual anchor).

2. **Swipe detection** (optional, requires GT911 multi-point or delta tracking): swipe
   down from top → open menu. Currently out of scope until touch driver is extended.

3. **Menu overlay cleanup:**
   - Show the book title in larger text (it's the main context anchor).
   - Add chapter name (pulled from `reader.getChapterTitle(currentChapter)`).
   - Add explicit "Home / Library" button as the fourth menu item (currently it just
     says "Library" — make it say "⇐ Library" and confirm with a "Leave book?" prompt
     if unsaved progress risk exists).

4. **Back navigation from TOC/Bookmarks/Settings:** Bottom bar already has "Back to
   Reading" label. Add a short-press of the physical top button (GPIO 21) as an
   alternative back gesture — currently it only triggers sleep on hold; a short tap
   (<600 ms) is unused.

**Files:** `main.cpp` (handleReaderTouch, handleTocTouch, handleBookmarksTouch,
handleSettingsTouch, handleMenuTouch, button polling in loop).

---

## 2. TOC / Chapter Picker Improvements

**Current state:** Spine-fallback TOC only (semantic nav/NCX disabled). Works but lists
every spine item, including cover pages, copyright pages, image-only items that produce
zero readable text.

**Problems:**
- Selecting a "cover" or "copyright" spine item loads a near-empty chapter →
  `[No readable text in this section]` message.
- No indication of which chapters have readable content.
- Large TOCs (400+ items) require many scroll pages.

**Proposed improvements:**

### 2a. Pre-filter empty/stub chapters in the TOC

Add a `uint8_t chapterWordCount` field to `TocEntry`. During `buildSpineFallbackToc`,
call `_parser.getChapterText(i)` briefly to count words (stop after 20 words found,
or after reading 4 KB). If word count is 0, mark the entry as a stub and exclude it
from the displayed TOC list. (Already reading the chapter for the title cache hit; no
extra I/O.)

Cache this information in `_toc[i].isEmpty` flag. In `drawTocScreen`, skip or visually
dim entries marked empty.

**Risk:** Adds ~1–5 ms per chapter on book open for 10–400 chapters → acceptable for
books <100 chapters; skip the heuristic for books >200 chapters and only filter on
TOC display tap.

### 2b. Jump-to-letter index (large TOCs)

For books with >50 TOC entries, draw a right-side letter strip (A–Z) that lets the user
jump to the first entry whose label starts with that letter. This is the standard mobile
pattern. Requires ~20 px on the right margin and touch-zone math adjustment.

### 2c. Chapter progress indicator

Next to each TOC entry, show a small "●" dot if that chapter has been read (i.e., the
saved progress chapter > this chapter index). Draw it in gray (color 10). This gives
visual "you are here + you've been here" context.

**Files:** `epub.cpp` (`TocEntry` struct, `buildSpineFallbackToc`), `epub.h`,
`main.cpp` (`drawTocScreen`, `handleTocTouch`).

---

## 3. Bookmark Polish

**Current state:** Bookmarks work (add via long-press, list via menu, tap to jump,
tap "x" to delete). Labels are auto-generated as "Ch N, Pg M - ChapterTitle".

**Problems:**
- The "x" delete target (rightmost 80 px) is small and has no visual affordance on
  e-ink (no hover state).
- No sort order — bookmarks are in insertion order.
- No way to rename a bookmark.
- When jumping to a bookmarked page, the full `display_update()` (6-cycle clear) is
  triggered even though the reader was just showing the bookmarks list (light content).

**Proposed improvements:**

### 3a. Confirm-to-delete

On first tap of "x": show the entry highlighted (re-draw with inverted background,
fast partial update). On second tap of "x" within 2 s: delete. Tap elsewhere: cancel.
Prevents accidental deletion.

### 3b. Visual affordance for delete

Replace "x" with a trash icon rendered as a 3-line pixel art glyph (can be drawn with
`display_draw_hline`/`display_draw_vline` primitives — no new font glyph needed).
Make the tap zone 60 px wide instead of 80 px.

### 3c. Bookmark sort

Sort by chapter + page on `saveProgress` / `loadProgress`. Ensures bookmarks always
appear in reading order regardless of insertion order.

### 3d. "Loading chapter..." before bookmark jump

Same as the TOC fix — bookmark jumps call `jumpToChapter` which can block. Add the same
loading indicator in `handleBookmarksTouch` before the `jumpToBookmark` call (which
internally calls `loadChapter`).

**Files:** `main.cpp` (`drawBookmarksScreen`, `handleBookmarksTouch`),
`reader.cpp` (`addBookmark`, `saveProgress`).

---

## 4. Persistence Hardening

**Current state:** Progress is saved atomically (tmp file → rename) using
`storage_write_text_atomic`. Bookmarks are serialized inside the same JSON.

**Gaps:**

### 4a. Save-on-page-turn (auto-save)

Currently `saveProgress()` is called only on `closeBook()`, `addBookmark()`, and
`removeBookmark()`. If the device runs out of battery mid-read (without triggering
deep-sleep), progress is lost.

Add auto-save every N page turns. Suggest N = 5 (configurable in settings).
`saveProgress()` is fast (~5 ms for a 1 KB JSON write with the atomic pattern). Safe
to call on every page turn, even every turn.

Implementation: increment `_pagesSinceLastSave` on each `nextPage`/`prevPage`; call
`saveProgress()` when it hits the threshold.

### 4b. Corrupt progress file recovery

`loadProgress()` silently resets to chapter 0 / page 0 on JSON parse error. Consider
falling back to the `.tmp` file if the main file is corrupt (the tmp survives atomic
write failures in some crash scenarios). Also log the reset with `Serial.printf`.

### 4c. SD card remount on error

If an `SD.open()` fails in a context where it previously succeeded (e.g., the SD card
was briefly ejected/reseated), the SD library does not automatically remount. Add a
`library_remount()` helper that calls `SD.begin(SD_CS)` again and retry once before
showing an error.

**Files:** `reader.cpp` (`nextPage`, `prevPage`, `loadProgress`),
`storage_utils.cpp`, `library.cpp`.

---

## 5. Library Improvements

**Current state:** Library lists `.epub` files from `/books/`. Poster view shows cover
images (rendered via `cover_renderer.cpp`). "Continue Reading" banner shows most-recently
read book (via `last_read` NVS monotonic counter).

**Gaps:**

### 5a. Sort order options

Add a library sort toggle: alphabetical by title vs. recently-read (default). Currently
the list is in SD filesystem order (effectively alphabetical on FAT32).

Implement: `library_scan()` already returns `std::vector<BookInfo>`. Pass a sort key
enum through `Settings`. Sort in `drawLibraryScreen` before rendering. Store setting
in `settings_save()`.

### 5b. "Mark as finished" / shelf

Add a `finished` boolean to `BookInfo` / progress JSON. Books marked finished appear
with a visual "✓" indicator in the library and are sorted to the bottom of the list
in the default sort order.

Trigger: long-press on a book in the library (currently unhandled).

### 5c. Delete book from device

Long-press on a book in the library → confirm dialog → `SD.remove(path)` removes the
EPUB and its progress file. This avoids requiring a PC connection to manage storage.

**Files:** `library.cpp`, `library.h`, `main.cpp` (`drawLibraryScreen`,
`handleLibraryTouch`), `settings.h`, `settings.cpp`.

---

## 6. Performance Groundwork

### 6a. Async/deferred pre-caching

`preCacheNext()` is called in `loop()` when `!needsRedraw`. It's synchronous and can
block for 2–10 s for large chapters. During this time, touch is unresponsive.

**Fix:** Track whether pre-caching is in progress. Use a state machine:
`PRECACHE_IDLE` → `PRECACHE_LOADING_NEXT` → `PRECACHE_LOADING_PREV` → `PRECACHE_IDLE`.
Each loop iteration does a bounded chunk of work (e.g., call `wrapText` for 200 ms then
return). This requires splitting `wrapText` into an iterable / resumable form, which is
complex. A simpler stopgap: call `yield()` (already done via the millis-based fix) and
accept that pre-caching blocks but WDT won't fire.

Longer term: run pre-caching on Core 0 (Arduino main loop runs on Core 1 on ESP32-S3).
Use a FreeRTOS task with a mutex on `_chapterCache`. This is the cleanest solution.

### 6b. Chapter title cache persistence

`_chapterTitleCache` is rebuilt from ZIP every boot. For a 408-chapter book, this means
408 ZIP reads during `buildSpineFallbackToc`. Store the title cache in the progress JSON
alongside bookmarks, keyed by chapter index. On load, if cache size matches spine size,
skip the ZIP reads entirely.

**Estimated boot time savings for large books:** 5–30 s.

### 6c. Display update latency

The `display_update()` path uses 6 EPD clear cycles (very conservative ghost removal).
Empirically, 2–3 cycles are sufficient after a fast-update (TOC/menu/settings screens).
Add a `display_update_medium()` function: `epd_clear_area_cycles(area, 2, 40)`.
Use it for chapter transitions (after TOC/bookmark jumps) instead of the heavy 6-cycle
version. Reserve the 6-cycle full update for after 10+ fast page turns.

**Files:** `display.cpp`, `display.h`, `main.cpp`.

---

## 7. Future: Inline EPUB Image Rendering

**Current state:** `stripHtml()` discards all tags including `<img>`. Images in EPUBs
are invisible.

**Design approach (phased):**

### Phase A — Cover images only (done)
Already implemented via `cover_renderer.cpp` which reads the cover image path from
`EpubParser::getCoverImagePath()` and renders it in the library poster view.

### Phase B — Inline images in reader (future)

**Challenge:** The reader pipeline is text-only (`getChapterText` → `wrapText` →
line-based layout). Adding images requires a mixed text+image layout engine.

**Proposed architecture:**

1. Introduce a `LayoutBlock` union type:
   ```
   struct TextBlock { std::vector<String> lines; };
   struct ImageBlock { String zipPath; int displayW; int displayH; };
   using LayoutBlock = std::variant<TextBlock, ImageBlock>;
   ```
2. Replace `_wrappedLines` + `_pages` with `std::vector<LayoutBlock>` and a separate
   paginator that fills pages with a mix of text lines and image blocks (using the image
   height in pixels to account for space).
3. `stripHtml` is replaced with a streaming parser that emits `TextBlock` and
   `ImageBlock` events.
4. In `drawReaderScreen`, render each block: text blocks use `display_draw_text`,
   image blocks call the cover renderer (already supports grayscale scaling).

**Constraints:**
- PSRAM budget: each inline image needs temporary PSRAM for decode (~2–4 MB for a
  full-page illustration). Fine for one image at a time; watch for OOM with image-heavy
  books (manga, illustrated children's books).
- Max image per page: one block image per page. Multi-image pages collapse to the first.
- Progressive JPEG: JPEGDEC does not support progressive JPEG. Convert to baseline on
  upload (recommend adding a pre-processing step to the WiFi upload server).

**Estimated effort:** 3–5 days of firmware work. Prerequisite: Phase B paginator
refactor is a significant architectural change — plan it as a separate branch.

---

## Implementation Priority Order

| Priority | Item | Effort | Impact |
|----------|------|--------|--------|
| 1 | Auto-save on page turn (§4a) | 30 min | High — prevents progress loss |
| 2 | Loading indicator for bookmark jumps (§3d) | 30 min | High — mirrors TOC fix |
| 3 | `display_update_medium()` for transitions (§6c) | 1 h | High — reduces EPD clear time |
| 4 | Chapter title cache in progress JSON (§6b) | 2 h | High — eliminates 408-zip-reads |
| 5 | TOC pre-filter empty chapters (§2a) | 2 h | Medium — better TOC quality |
| 6 | Library sort toggle (§5a) | 2 h | Medium — UX quality |
| 7 | Physical button short-press = back (§1, point 4) | 1 h | Medium — discoverability |
| 8 | Bookmark confirm-to-delete (§3a) | 2 h | Medium — prevents data loss |
| 9 | Core 0 pre-caching (§6a) | 4 h | Medium — removes blocking |
| 10 | Inline EPUB image rendering (§7) | 3–5 days | Low/future |
