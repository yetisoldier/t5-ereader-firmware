# T5 E-Reader Firmware — Issue Log & Handoff

**Date:** 2026-03-31
**Project:** `/home/yetisoldier/.openclaw/workspace/t5-ereader-firmware`
**Hardware:** LilyGo T5 4.7" V2.4 (ESP32-S3-WROOM-1-N16R8, 16MB flash, 8MB OPI PSRAM)
**Platform:** PlatformIO, espressif32@6.4.0, Arduino framework

---

## ✅ RESOLVED — Build now compiles cleanly

### Issue 1 — Font struct mismatch (FIXED)
**Problem:** Code used `xAdvance` / `yAdvance` (Adafruit GFX field names), but LilyGo-EPD47 uses `advance_x` / `advance_y` and its own `get_glyph()` function.
**Fix:** `src/display.cpp` — replaced direct glyph array access with `get_glyph(font, codepoint, &glyph)` and `glyph->advance_x` / `font->advance_y`.

### Issue 2 — miniz include path (FIXED)
**Problem:** `src/epub.cpp` tried to include `esp32s3/rom/miniz.h` (ROM miniz), which is not on the compiler's default include path.
**Fix:** Downloaded miniz 2.1.0 from richgel999/miniz and added `src/miniz.h` + `src/miniz.c` directly to the project.

### Build output (clean)
```
RAM:   [=         ]  14.3% (used 46700 bytes from 327680 bytes)
Flash: [=         ]  12.0% (used 1003437 bytes from 8388608 bytes)
```

---

## ✅ RESOLVED — Touch not responding (FIXED)

### Root cause: GT911 not waking / initializing
**Fix applied:**
- `config.h`: Changed `TOUCH_INT_PIN` from `-1` to `47`
- `touch.cpp`: Added INT pin wake-up, I2C address scanning, coordinate transforms
- Verified working: touch events detected, books selectable

---

## ✅ RESOLVED — EPUB files fail to open (ZIP: cannot open)

### Root cause: VFS mount path mismatch
**Problem:** `ZipReader::open()` uses POSIX `fopen()` with SD-relative paths like `/books/mybook.epub`. On ESP32 Arduino, the SD card is mounted at `/sd` in the VFS (Virtual File System). Arduino's `SD.open()` handles this prefix transparently, but raw `fopen()` does not — it needs `/sd/books/mybook.epub`.

**Fix applied (`src/epub.cpp`):**
- `ZipReader::open()` now prepends `/sd` to the path if it doesn't already start with `/sd`
- Added diagnostic logging showing both the caller path and the VFS path
- Added `errno` reporting for any remaining open failures

**Verified working (serial output):**
```
ZIP: opening /books/...Odd Thomas....epub (vfs: /sd/books/...Odd Thomas....epub)
ZIP: found 471 entries
EPUB: title = The Odd Thomas Series 7-Book Bundle
EPUB: 408 chapters in spine

ZIP: opening /books/...Ender's Game....epub (vfs: /sd/books/...Ender's Game....epub)
ZIP: found 29 entries
EPUB: title = Enders Game 1 - Ender's Game
EPUB: 18 chapters in spine

Library: 2 books found
```

---

## ✅ RESOLVED — Upload permission (dialout group)

User `yetisoldier` is now in the `dialout` group. Upload works via:
```bash
pio run -t upload --upload-port /dev/ttyACM0
```

---

## Ghost OS-Style UI Enhancements (added 2026-03-31)

### "Continue Reading" banner
- Library screen shows a tappable "Continue Reading: [title]" banner at the top when there's a book with saved progress
- Tapping the banner opens that book directly to the last-read position

### Reading progress in library
- Each book in the library list shows a progress percentage on the right (based on chapter position)
- Progress is loaded from the per-book `.json` save files in `/books/.progress/`

### Reader header
- Reader screen now shows the book title (gray, subtle) in a mini header bar
- Battery percentage displayed on the right
- Thin separator line below for clean visual hierarchy

### Reader footer improvements
- Overall reading progress percentage (based on chapters) shown centered in the footer
- Chapter counter on the left, page counter on the right, progress % in the middle

---

## Remaining Limitations

### No font size control
The firmware uses a single fixed font (FiraSans from LilyGo-EPD47). Font size adjustment would require loading multiple font sizes or implementing a scaling approach.

### Single-level progress tracking
Progress is saved per-book as chapter + page. There's no timestamp on progress files, so "Continue Reading" picks the first book with progress rather than the most recently read.

### Long filenames
Books with very long paths/filenames work but get truncated in the UI. The full path is preserved internally.

### No table of contents navigation
The reader supports linear chapter-by-chapter reading. There's no chapter picker / TOC view yet.

### No image rendering
EPUB images are stripped during HTML→text conversion. Only text content is displayed.

---

## File Map

```
t5-ereader-firmware/
├── platformio.ini          (espressif32@6.4.0, arduino, 16MB flash)
├── partitions.csv          (custom partition table)
├── src/
│   ├── main.cpp            (setup/loop, state machine, UI drawing)
│   ├── config.h            (pin definitions, WiFi creds, layout constants)
│   ├── display.cpp/.h      (EPD wrapper — portrait framebuffer, rotation)
│   ├── epub.cpp/.h         (ZIP/EPUB parser — VFS path fix applied)
│   ├── miniz.h/.c          (miniz 2.1.0 — bundled for inflate/deflate)
│   ├── library.cpp/.h      (SD card book scanning + progress loading)
│   ├── reader.cpp/.h       (page rendering, text wrapping, progress save/load)
│   ├── touch.cpp/.h        (GT911 touch input with coordinate transforms)
│   ├── battery.cpp/.h      (ADC voltage monitoring)
│   └── wifi_upload.cpp/.h  (WiFi OTA upload server)
└── FIRMWARE_ISSUES.md      (this file)
```
