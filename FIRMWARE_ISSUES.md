# T5 E-Reader Firmware — Issue Log & Handoff

**Date:** 2026-04-04
**Project:** `/home/yetisoldier/.openclaw/workspace/t5-ereader-firmware`
**Hardware:** LilyGo T5 4.7" V2.4 (ESP32-S3-WROOM-1-N16R8, 16MB flash, 8MB OPI PSRAM)
**Platform:** PlatformIO, espressif32@6.4.0, Arduino framework
**Version:** v0.3.1

---

## Build Status

```
RAM:   [=         ]  14.3% (used 46700 bytes from 327680 bytes)
Flash: [=         ]  12.0% (used 1003437 bytes from 8388608 bytes)
```

---

## Resolved Issues

### Font struct mismatch (FIXED)
Code used `xAdvance` / `yAdvance` (Adafruit GFX field names), but LilyGo-EPD47 uses `advance_x` / `advance_y`. Fixed in `src/display.cpp`.

### miniz include path (FIXED)
Downloaded miniz 2.1.0 and added `src/miniz.h` + `src/miniz.c` directly to project.

### Touch not responding (FIXED)
GT911 INT pin wake-up added. `config.h`: `TOUCH_INT_PIN` changed to `47`. Touch events working.

### EPUB files fail to open (FIXED)
`ZipReader::open()` now prepends `/sd` to paths for VFS compatibility.

### Upload permission (FIXED)
User in `dialout` group. Upload via `pio run -t upload --upload-port /dev/ttyACM0`.

### Grayscale inversion (FIXED v0.3.0)
Cover and inline images now render with correct polarity (0=black, 15=white).

### Poster placeholder detection (FIXED v0.3.0)
Multi-layer statistical heuristic detects broken/placeholder images; fallback to covers-off card layout.

### Startup splash version (FIXED v0.3.0)
Version text now rendered dynamically; splash bitmap is static artwork only.

---

## Current Features (v0.3.1)

### Reading
- 7 font sizes: XS, S, M, ML, L, XL, XXL
- Serif toggle: Fira Sans or Noto Serif
- TOC navigation (chapter picker overlay)
- Bookmarks with quick jump
- Progress persistence per-book

### Library
- Filter tabs: ALL / NEW / READING / DONE
- "Continue Reading" banner for in-progress books
- Progress percentage displayed

### OPDS / Gutenberg
- Browse and download from Calibre servers or Project Gutenberg
- Genre browser for Gutenberg (no keyboard required)
- Download progress indicator

### System
- OTA updates from GitHub Releases (Settings → Check for Update)
- WiFi upload server with QR code for easy access
- Sleep image rotation from `/sleep` folder
- Physical button: single=next, double=prev, long=sleep

---

## Known Limitations

### Single-level progress tracking
Progress is saved per-book as chapter + page. No timestamp, so "Continue Reading" picks the first book with progress rather than the most recently read.

### Long filenames
Books with very long paths/filenames work but get truncated in the UI. Full path preserved internally.

### No inline image rendering
EPUB images are stripped during HTML→text conversion. Only text content is displayed. (Covers and sleep images render correctly.)

### No regression tests
No automated CI for push/PR. Broken commits can land on `main` undetected.

---

## File Map

```
t5-ereader-firmware/
├── platformio.ini           (espressif32@6.4.0, arduino, 16MB flash)
├── partitions.csv           (custom partition table)
├── CHANGELOG.md             (version history)
├── README.md                 (usage, gestures, screenshots)
├── docs/
│   └── screenshots/          (device photos for README)
├── tools/
│   └── generate_gnome_splash.py  (splash bitmap generator)
├── include/
│   ├── config.h.example      (config template)
│   ├── ota_update.h
│   └── gnome_splash.h        (generated splash bitmap)
├── src/
│   ├── main.cpp              (setup/loop, state machine, UI dispatch)
│   ├── config.h              (pin definitions, WiFi, layout constants)
│   ├── display.cpp/.h        (EPD wrapper — portrait, rotation)
│   ├── epub.cpp/.h           (ZIP/EPUB parser)
│   ├── miniz.h/.c            (miniz 2.1.0 bundled)
│   ├── library.cpp/.h        (SD scanning, progress loading)
│   ├── reader.cpp/.h         (page rendering, progress)
│   ├── settings.cpp/.h        (settings persistence)
│   ├── touch.cpp/.h           (GT911 input, transforms)
│   ├── battery.cpp/.h         (ADC voltage)
│   ├── sleep_image.cpp/.h     (sleep screen rendering)
│   ├── cover_renderer.cpp/.h  (book covers)
│   ├── inline_image.cpp/.h    (EPUB inline images)
│   ├── image_tone.h           (gamma/brightness correction)
│   ├── wifi_upload.cpp/.h     (WiFi upload server)
│   ├── ota_update.cpp/.h      (OTA update flow)
│   ├── opds_store.cpp/.h      (OPDS catalog browsing)
│   ├── opds_client.h           (OPDS HTTP client)
│   └── storage_utils.h         (SD card helpers)
└── FIRMWARE_ISSUES.md        (this file)
```

---

## Development Notes

### Flashing
```bash
pio run -t upload --upload-port /dev/ttyACM0
```

### Serial Monitor
```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

### OTA Updates
1. Tag a release: `git tag v0.x.x && git push --tags`
2. GitHub Actions builds `firmware.bin`
3. Device: Settings → Check for Update