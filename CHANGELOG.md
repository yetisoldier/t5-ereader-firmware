# Changelog

## v0.4.2 — 2026-04-25

### Changed
- Settings screen bottom layout now places **Reset Defaults** lower for cleaner separation from the Reading/Device page selector
- OTA screen flow now performs the update check outside the draw path and cleans up WiFi state consistently when leaving the OTA screen

### Fixed
- **OTA return freeze:** Returning to Settings after checking for updates no longer freezes the device
- **OTA WiFi failure recovery:** Failed WiFi connection attempts now return a clear error state and shut WiFi down cleanly on exit
- **Settings redraw path:** Settings refresh handling now uses a one-shot softer transition only when entering from another screen, then returns to normal full redraws
- **Version label rendering:** Manual release builds now display the expected firmware version on-device
- **Inline PNG rendering restored:** Hardened file-backed PNG decode/render path is back in service on-device

## v0.4.1 — 2026-04-25

### Changed
- Inline EPUB image handling now prioritizes stability on real hardware while the PNG render path is being reworked

### Fixed
- **Inline PNG reboot loop:** PNG images embedded in some EPUBs no longer hard-reset the device during page render
- **Graceful image fallback:** Unsupported or temporarily blocked inline PNG renders now fall back cleanly instead of crashing the reader
- **Crash diagnostics:** Added persistent debug trace markers around boot, wake, display, and reader rendering to speed up hardware fault isolation
- **Touch release safety:** Reader touch handling now avoids using stale release coordinates during gesture/tap resolution

## v0.4.0 — 2026-04-04

### Added
- **GNOME boot splash:** New splash screen artwork displayed on cold start
- **Line spacing settings:** Choose from Compact, Normal, Relaxed, Spacious, or Extra line heights in Settings
- **Library cover pre-caching:** Poster view now warms a 16-entry cover cache for smoother scrolling
- **Random sleep images:** Sleep screen rotates through images from `/sleep` directory with fallback to built-in

### Changed
- **UI refactoring:** Extracted `ui_library.cpp`, `ui_settings.cpp`, and `state.h` from main.cpp for cleaner architecture
- **Font sizes reduced:** Simplified from 7 options (XS–XXL) to 5 options (XS, S, M, M-L, L)
- **Settings split into two pages:** Navigation and display settings now organized across paginated screens
- **Settings refresh optimization:** Settings screen uses faster `display_update_medium()` instead of full refresh
- **Settings tab outline fix:** Tabs now use outline-only rendering instead of filled boxes

### Fixed
- **Critical sleep/power freeze:** Fixed device freeze when USB disconnected — all `Serial.print`/`flush` calls in sleep path now guarded with `if (Serial)` to prevent blocking when USB is not attached
- **Crash guard on wake:** Reopening books after wake now has crash protection to prevent boot loops from corrupted state
- **Display hold during sleep:** `display_update_sleep()` now preserves panel latch state correctly during sleep transitions

---

## v0.3.1 — 2026-04-03

### Added
- Seven font size levels: XS, S, M, ML, L, XL, XXL (replaces Small/Medium/Large)
- Serif font toggle: choose between Fira Sans (sans-serif) and Noto Serif in Settings
- OPDS client: browse and download books from Calibre servers or Project Gutenberg
- Gutenberg genre browser with category picker (no keyboard required)
- Library filter tabs: ALL / NEW / READING / DONE
- Store button in library footer for OPDS access
- QR code on the WiFi upload screen for quick URL access
- Settings layout: Reset Defaults and Firmware version now displayed on a single line, properly spaced and vertically centered above the Back bar

### Changed
- Settings menu bottom row reformatted — Reset Defaults (left) and Firmware version (right) on one line

---

## v0.3.0 — 2026-04-02

### Added
- New gnome boot splash artwork (540×540, replaces previous placeholder)
- Updated screenshot gallery in README with current device photos (library, reader, settings, sleep, wake)

### Changed
- Boot splash bitmap polarity corrected — black/white pixels now render accurately on the e-ink panel
- Splash art dimensions updated to 540×540; version and status text rendered dynamically below the artwork

### Fixed
- Grayscale inversion corrected for cover art and inline EPUB images
- Poster placeholder detection improved with multi-layer statistical heuristic and fingerprint-based check
- Universal cover quality guards added (size, dimensions, aspect ratio, decode failure)
- Brightened image rendering pipeline for cleaner display output
- Removed stale broken screenshot reference from README

---

## v0.2.1 — 2026-04-02

### Added
- OTA update path in Settings with GitHub Releases version check, download progress, install flow, and reboot on success
- Dual OTA app partitions plus a GitHub Actions release workflow that builds `firmware.bin` from release tags
- Built-in cold-boot splash screen and wake feedback banner before the restored UI redraws
- Automatic SD folder bootstrap for `/books`, `/books/.progress`, `/books/.linecache`, and `/sleep`

### Changed
- Top physical button behavior is now unified on GPIO 21: long press sleeps, short press moves forward, and double press moves back
- Sleep image handling now scans `/sleep`, supports `.png`, `.jpg`, and `.jpeg`, rotates images, retries the next image after a bad file, and falls back cleanly to the built-in sleep screen
- Sleep image rendering now handles larger source images more reliably by allocating PNG line buffers to the actual image width and rejecting oversized files instead of failing unpredictably
- Reader wake/resume now restores state more cleanly, including immediate redraw after wake and safer fallback to the library if a saved book cannot reopen
- Display refresh behavior was tuned to reduce ghosting and improve transitions for library, settings, TOC, bookmarks, wake, and sleep-image rendering
- Settings flow now includes the firmware version on screen and a firmware update entry
- Storage and boot serial logs are more explicit about SD mount status and folder readiness
- WiFi upload guidance and docs now match the current `/books` and `/sleep` layout

### Fixed
- Corrected the configured sleep-image folder from `/sleep_images` to `/sleep`
- Fixed sleep-image grayscale conversion so rendered custom images display with the intended polarity
- Fixed release-version consistency by allowing tagged release builds to inject `FIRMWARE_VERSION` from the release tag while keeping the local default in `config.h.example`

## v0.1.0 — 2026-04-01

Initial public release.

### Features
- **EPUB reader** with chapter-by-chapter navigation, text pagination, and word wrapping
- **Library screen** with list view and poster/cover art view, paginated navigation
- **Continue Reading** banner highlighting last-opened book with progress
- **Progress badges** showing read percentage on each book in the library
- **Table of Contents (TOC)** navigation overlay
- **Bookmarks** — add, remove, and jump to bookmarks within a book
- **Reading statistics** — tracks total reading time and pages read per book
- **Font size presets** — Small, Medium, Large with adjusted line spacing and margins
- **Settings screen** with persistent JSON storage on SD card
  - Font size, sleep timeout, refresh interval, page numbers, battery display
  - Tap zone layout (left/center/right or top/mid/bottom)
  - Library view mode (list or poster)
- **WiFi upload mode** — built-in web server for wireless book management
  - Upload EPUB files and sleep images from any browser
  - Delete books from the web interface
  - Search within the currently open book
  - Update WiFi credentials from the web UI
- **Inline EPUB image rendering** with SD-cached decoded lines
- **Partial e-ink refresh** for fast page turns with periodic full cleanup
- **Deep sleep** power management with configurable timeout
- **Sleep/wake** via GPIO 21 top button (hold ~600ms to sleep)
- **Touch wake** from deep sleep via GT911 interrupt
- **Battery percentage** display in reader header
- **Metadata caching** for fast library scans on subsequent boots
- **Portrait-first UI** (540x960) rotated from landscape hardware (960x540)
- **Custom sleep screen** images loaded from SD card
- **Atomic file writes** for crash-safe settings and progress saving

### Hardware Support
- LilyGo T5 4.7" V2.3 and V2.4 (PCB 2024-01-15)
- ESP32-S3-WROOM-1-N16R8 (16MB flash, 8MB OPI PSRAM)
- ED047TC1 4.7" e-ink display via epdiy/LilyGo-EPD47
- GT911 capacitive touch (I2C, auto-detect 0x5D/0x14)
- PCF8563 RTC
- SD card via SPI
