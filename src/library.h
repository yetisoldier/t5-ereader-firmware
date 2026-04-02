#pragma once

#include <Arduino.h>
#include <vector>

struct BookInfo {
    String title;
    String filepath;
    int progressChapter = 0;   // last-read chapter index
    int progressPage = 0;      // last-read page index
    int totalChapters = 0;     // chapters in book
    bool hasProgress = false;  // true if a progress file exists
    uint32_t lastReadOrder = 0; // monotonic counter for "most recently read"
    size_t fileSize = 0;       // file size for cache validation
    bool hasCover = false;
    String coverPath;
    bool posterCoverFailed = false; // runtime-only fallback when poster rendering is broken
};

bool library_init();
std::vector<BookInfo> library_scan();

// Returns index of the most-recently-read book (with progress), or -1
int library_find_current_book(const std::vector<BookInfo>& books);
