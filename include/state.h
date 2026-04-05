#pragma once

// App state machine
enum AppState {
    STATE_BOOT,
    STATE_LIBRARY,
    STATE_READER,
    STATE_WIFI,
    STATE_MENU,         // redesigned reader overlay menu
    STATE_GOTO,         // go to approximate page or percentage
    STATE_TOC,          // table of contents
    STATE_BOOKMARKS,    // bookmark list
    STATE_SETTINGS,     // settings page
    STATE_OTA_CHECK,    // OTA update check / download
    STATE_OPDS_BROWSE,  // Browsing OPDS catalog
    STATE_OPDS_DOWNLOAD // Download in progress
};
