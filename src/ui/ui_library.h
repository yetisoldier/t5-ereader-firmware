#pragma once

#include <Arduino.h>
#include <vector>
#include "../library.h"
#include "../include/state.h"

// Library UI drawing and touch handling
// Stateless - all state is managed by the caller (main.cpp)

// Draw the library screen
// Parameters:
//   books - list of all books
//   scroll - current scroll position (modified on page change)
//   filter - current filter (ALL, NEW, READING, DONE)
//   filteredIndices - pre-filtered book indices
//   firstDraw - true if this is the first draw after splash (triggers full refresh)
void ui_library_draw(
    std::vector<BookInfo>& books,
    int& scroll,
    int filter,
    const std::vector<int>& filteredIndices,
    bool& firstDraw
);

// Handle touch on library screen
// Parameters:
//   x, y - touch coordinates
//   books - list of all books
//   scroll - current scroll position (modified on page change)
//   filter - current filter (modified on filter tab change)
//   filteredIndices - pre-filtered book indices (modified on filter change)
// Returns:
//   STATE_READER if a book was opened, STATE_SETTINGS if settings clicked,
//   STATE_OPDS_BROWSE if store clicked, or current state if no navigation
AppState ui_library_touch(
    int x, int y,
    std::vector<BookInfo>& books,
    int& scroll,
    int& filter,
    std::vector<int>& filteredIndices
);