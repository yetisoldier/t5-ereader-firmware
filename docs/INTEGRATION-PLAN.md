# GhostPaper Integration Plan
**Author:** Bob (Architect Agent)  
**Date:** 2026-04-02  
**Status:** Approved — Phase 1 in progress

---

## Overview

Integrate the best features from [GhostPaper](https://github.com/mistrysiddh/GhostPaper) into our EPUB firmware. PIN security explicitly excluded per Eric's decision.

---

## Feature Decisions

| Feature | Decision | Phase | Complexity | Flash Cost |
|---------|----------|-------|------------|------------|
| OPDS Book Store (Gutenberg + Calibre) | ✅ INTEGRATE | 1 | HIGH | ~40-60KB |
| Library Filters (ALL/NEW/READING/FINISHED) | ✅ INTEGRATE | 1 | LOW | <5KB |
| QR Code WiFi Upload Entry | ✅ INTEGRATE | 1 | LOW | ~8KB |
| Expanded Font Sizes (6-7 presets) | ✅ INTEGRATE | 2 | MEDIUM | ~120-180KB |
| Serif/Sans-serif Font Toggle | ✅ INTEGRATE | 2 | MEDIUM | ~100-180KB |
| Newspaper Dashboard (clock/weather/quotes) | ⏸️ DEFER | 3 | MEDIUM-HIGH | ~15KB |
| 6-Digit PIN Security | ❌ SKIP | — | — | — |

---

## Phase 1: OPDS Store + Library Filters + QR Code

### New Files
- `src/opds_client.h` / `src/opds_client.cpp` — Generic OPDS XML catalog parser and EPUB downloader
- `src/opds_store.h` / `src/opds_store.cpp` — Store UI, server management, browse/download state

### Modified Files
- `src/main.cpp` — Add `STATE_OPDS_BROWSE`, `STATE_OPDS_DOWNLOAD`, `STATE_OPDS_SERVERS` to AppState enum; wire up state transitions
- `src/library.cpp` / `src/library.h` — Add filter tabs (ALL/NEW/READING/FINISHED); filter logic on BookInfo
- `src/wifi_upload.cpp` — Add QR code generation and display on the upload screen
- `src/settings.h` / `src/settings.cpp` — Add `opdsActiveServer` field
- `platformio.ini` — Add `mathertel/tinyxml2` and `ricmoo/QRCode` dependencies

### New AppState Values
```cpp
STATE_OPDS_BROWSE,      // Browsing OPDS catalog
STATE_OPDS_DOWNLOAD,    // Download in progress (shows progress bar)
STATE_OPDS_SERVERS,     // Server configuration screen
```

### Default OPDS Servers
Stored in `/books/.opds_servers.json` on SD card:
```json
[
  {
    "name": "Project Gutenberg",
    "baseUrl": "https://www.gutenberg.org/ebooks/search.opds/?sort_order=downloads",
    "username": "",
    "password": "",
    "isDefault": true
  },
  {
    "name": "My Calibre",
    "baseUrl": "",
    "username": "",
    "password": "",
    "isDefault": false
  }
]
```

### Key Data Structures

```cpp
struct OpdsEntry {
    String title;
    String author;
    String acquisitionUrl;   // Direct EPUB download URL
    String detailUrl;        // OPDS detail feed URL (for nested resolve)
    String mimeType;         // "application/epub+zip"
    bool   isNavigation;     // true = subcatalog link, false = book
    String navUrl;           // URL if navigation entry
};

struct OpdsServer {
    String name;
    String baseUrl;
    String username;
    String password;
    bool   isDefault;
};

struct OpdsStoreState {
    std::vector<OpdsEntry> entries;
    std::vector<String>    navStack;      // Breadcrumb for back nav
    String                 currentUrl;
    int                    scrollOffset;
    int                    serverIndex;
    String                 statusMsg;
    bool                   downloading;
};
```

### OpdsClient API

```cpp
class OpdsClient {
public:
    std::vector<OpdsEntry> fetchCatalog(const String& url,
                                         const String& username = "",
                                         const String& password = "");
    bool downloadEpub(const String& url,
                      const String& targetPath,
                      std::function<void(int pct)> onProgress = nullptr);
    String resolveEpubUrl(const String& detailUrl, int maxDepth = 4);
    String getNextPageUrl() const;
    String getLastError() const;
private:
    String _nextPageUrl;
    String _lastError;
    static constexpr const char* EPUB_MIME = "application/epub+zip";
};
```

### OPDS Store UI Layout (540×960 portrait)
```
┌─────────────────────────────┐ y=0
│ ■ OPDS Store    [◁ Back]    │ Header (66px)
├─────────────────────────────┤ y=66
│ [Gutenberg ▾] [Calibre]     │ Server tabs (50px)
├─────────────────────────────┤ y=116
│ ┌─────────────────────────┐ │
│ │ Title of Book           │ │ Entry card (120px each)
│ │ Author Name    [GET]    │ │
│ └─────────────────────────┘ │
│ ┌─────────────────────────┐ │
│ │ Another Book Title      │ │ ~6 visible entries
│ │ Author Two   [ON DEVICE]│ │
│ └─────────────────────────┘ │
│         (scrollable)        │
├─────────────────────────────┤ y=910
│   ◁ Prev    3/12   Next ▷  │ Footer (50px)
└─────────────────────────────┘ y=960
```

### Library Filter API

```cpp
enum LibraryFilter {
    FILTER_ALL,
    FILTER_NEW,        // No progress file
    FILTER_READING,    // Has progress, not finished
    FILTER_FINISHED,   // Progress at last chapter
};

std::vector<int> library_filter(const std::vector<BookInfo>& books,
                                 LibraryFilter filter);
```

### Critical Notes (EPUB vs GhostPaper .txt)
- GhostPaper OPDS filters for `text/plain` MIME — we filter for `application/epub+zip`
- GhostPaper uses Gutenberg-specific `.txt.utf-8` URL heuristics — we resolve via OPDS `rel="http://opds-spec.org/acquisition"` link walking
- GhostPaper downloads flat text — we handle binary EPUB ZIP files
- GhostPaper saves to `/` — we save to `/books/`
- Calibre OPDS uses nested catalogs (browse by author → titles → download) — `isNavigation` flag handles drill-down

---

## Phase 2: Font Expansion + Serif Toggle

### Approach
- Pre-rasterize 4 additional font sizes (NOT float scaling — incompatible with our GFX bitmap system)
- Total: 7 sizes (XS/S/M/ML/L/XL/XXL)
- Add second font family (Crimson or Merriweather) as serif option
- Add serif/sans toggle to Settings

### New Files
- `src/font_xsmall.h`, `src/font_smedium.h`, `src/font_mlarge.h`, `src/font_xlarge.h`
- `src/font_serif_small.h`, `src/font_serif_medium.h`, `src/font_serif_large.h`

### Modified Files
- `src/settings.h` — Add `int fontSizeLevel` (0-6), `bool serifFont`
- `src/reader.cpp` — Font selection logic based on settings
- `src/settings.cpp` — Persist new fields

---

## Phase 3: Newspaper Dashboard (Deferred)

### Planned Features
- Large clock + date display
- Weather widget via wttr.in
- Literary quotes via quotable.io (or static fallback)
- Currently-reading widget with progress bar and tap-to-resume
- Background WiFi fetch task

### New Files
- `src/dashboard.h` / `src/dashboard.cpp`

### New AppState
- `STATE_DASHBOARD`

### Notes
- Requires WiFi auto-connect or manual trigger
- Uses RTC (PCF8563) for time display — already have RTC driver
- Do not implement until Phases 1-2 are stable

---

## Resource Budget

| Phase | Est. Flash Added | Remaining Flash |
|-------|-----------------|-----------------|
| Baseline | 1,233KB (29.4%) | 2,961KB (70.6%) |
| Phase 1 | +~75KB | ~68.8% remaining |
| Phase 2 | +~350KB | ~60.5% remaining |
| Phase 3 | +~15KB | ~60.2% remaining |

RAM impact is transient during WiFi operations (~30-40KB during fetch, freed after). Well within 45% headroom.
