# Changelog

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
