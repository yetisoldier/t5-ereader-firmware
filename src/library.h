#pragma once

#include <Arduino.h>
#include <vector>

struct BookInfo {
    String title;
    String author;
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

enum LibraryFilter {
    FILTER_ALL,
    FILTER_NEW,        // No progress file
    FILTER_READING,    // Has progress, not finished
    FILTER_FINISHED,   // Progress at last chapter
};

bool library_init();
std::vector<BookInfo> library_scan();
void library_sort(std::vector<BookInfo>& books);
std::vector<int> library_filter(const std::vector<BookInfo>& books,
                                 LibraryFilter filter);

// Returns index of the most-recently-read book (with progress), or -1
int library_find_current_book(const std::vector<BookInfo>& books);
