#pragma once

#include <Arduino.h>
#include <vector>
#include "epub.h"

struct PageRange {
    int lineStart;
    int lineEnd;  // exclusive
};

struct Bookmark {
    int chapter;
    int page;
    String label;
};

// ChapterCache removed — wrapped lines are now stored on SD card
// to avoid heap exhaustion on chapters with 500+ lines.

class BookReader {
public:
    bool openBook(const char* filepath);
    void closeBook();

    const String& getTitle() const { return _title; }
    const String& getFilepath() const { return _filepath; }
    int  getCurrentPage() const { return _currentPage; }
    int  getTotalPages() const { return _totalPages; }
    int  getCurrentChapter() const { return _currentChapter; }
    int  getTotalChapters() const { return _parser.getChapterCount(); }
    int  getTocCount() const { return _parser.getTocCount(); }

    bool nextPage();
    bool prevPage();
    void jumpToChapter(int chapter);
    void restorePage(int page);  // Set page within current chapter (clamped)
    bool didChapterChange() const { return _chapterChanged; }  // true if last page turn crossed a chapter boundary

    const std::vector<String>& getPageLines() const;

    void saveProgress();
    void loadProgress();

    // Pre-cache removed — lines stored on SD card now

    // Bookmarks
    void addBookmark();
    void removeBookmark(int idx);
    bool jumpToBookmark(int idx);
    const std::vector<Bookmark>& getBookmarks() const { return _bookmarks; }
    bool isCurrentPageBookmarked() const;

    // Chapter title access (delegates to parser)
    String getChapterTitle(int index);
    String getTocLabel(int index);
    int getTocChapterIndex(int index);
    const std::vector<SpineItem>& getSpine() const { return _parser.getSpine(); }

    // For font size changes
    void recalculateLayout();

    // Last-read ordering
    uint32_t getLastReadOrder() const { return _lastReadOrder; }

    // Reading statistics
    uint32_t getTotalReadingTimeSec() const { return _totalReadingTimeSec; }
    uint32_t getTotalPagesRead() const { return _totalPagesRead; }
    void updateReadingTime();  // Call periodically to accumulate session time

    // Parser access for inline image rendering
    EpubParser& getParser() { return _parser; }

private:
    EpubParser _parser;
    String _title;
    String _filepath;
    int _currentChapter = 0;
    int _currentPage = 0;
    int _totalPages = 0;
    int _totalLines = 0;

    // Lines are stored on SD card to avoid heap exhaustion.
    // _lineOffsets[i] = byte offset in the cache file for line i.
    std::vector<uint32_t> _lineOffsets;  // ~2KB for 500 lines vs ~28KB for Strings
    std::vector<PageRange> _pages;
    std::vector<String> _currentPageLines;
    String _lineCachePath;  // SD path for current chapter's line cache

    int _linesPerPage = 1;
    int _maxLineWidth = 1;
    int _lineSpacing = 4;

    // Chapter caching disabled — lines live on SD card now, not RAM.
    // Pre-caching is no longer needed since SD reads are fast.

    // Bookmarks
    std::vector<Bookmark> _bookmarks;

    // Last-read order (monotonic counter)
    uint32_t _lastReadOrder = 0;

    // Auto-save counter: save every N page turns
    int _pageTurnsSinceSave = 0;
    static const int SAVE_EVERY_N_TURNS = 5;
    bool _chapterChanged = false;

    // Reading statistics
    uint32_t _totalReadingTimeSec = 0;
    uint32_t _totalPagesRead = 0;
    unsigned long _sessionStartMs = 0;
    unsigned long _lastTimeUpdateMs = 0;

    void loadChapter(int chapter);
    void updatePageLines();
    void paginateLines();
    void wrapTextToFile(const String& text);  // write wrapped lines to SD cache
    String readLineFromCache(int lineIndex);   // read single line from SD cache
};
