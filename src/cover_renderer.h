#pragma once

#include "library.h"

bool cover_can_render_poster(const BookInfo& book);
bool cover_render_poster(BookInfo& book, int x, int y, int w, int h);
void cover_cache_clear();
void cover_precache_page(std::vector<BookInfo>& books, const std::vector<int>& filteredIndices, int scroll, int cardsPerPage);
