#include "reader.h"
#include "display.h"
#include "config.h"
#include "settings.h"
#include "storage_utils.h"
#include "inline_image.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <Preferences.h>

static Preferences _prefs;

static const int MAX_WRAP_TEXT_CHARS = 120000;
bool BookReader::openBook(const char* filepath) {
    closeBook();
    if (!_parser.open(filepath)) return false;

    _filepath = String(filepath);
    _title = _parser.getTitle();

    recalculateLayout();
    _pageTurnsSinceSave = 0;
    _sessionStartMs = millis();
    _lastTimeUpdateMs = _sessionStartMs;
    loadProgress();
    loadChapter(_currentChapter);

    // Update last-read order (monotonic counter in NVS)
    _prefs.begin("ereader", false);
    _lastReadOrder = _prefs.getUInt("readOrder", 0) + 1;
    _prefs.putUInt("readOrder", _lastReadOrder);
    _prefs.end();

    return true;
}

void BookReader::recalculateLayout() {
    const Settings& s = settings_get();

    // Switch the actual font in the display module
    display_set_font_size(s.fontSize);

    // Margins and line spacing scale with font size
    int lineSpacing;
    int marginX;
    switch (s.fontSize) {
        case 0: lineSpacing = FONT_LINE_SPACING_SMALL;  marginX = FONT_MARGIN_X_SMALL;  break;
        case 2: lineSpacing = FONT_LINE_SPACING_LARGE;  marginX = FONT_MARGIN_X_LARGE;  break;
        default: lineSpacing = FONT_LINE_SPACING_MEDIUM; marginX = FONT_MARGIN_X_MEDIUM; break;
    }

    int bodyFontH = display_font_height();
    // The footer already reserves its own area, so only a tiny extra buffer is
    // needed above it now that draw-time spacing matches pagination.
    int usableHeight = display_height() - HEADER_HEIGHT - FOOTER_HEIGHT - MARGIN_Y * 2 - 4;
    int usableWidth  = display_width() - marginX * 2;

    _lineSpacing = lineSpacing;
    _linesPerPage = usableHeight / (bodyFontH + lineSpacing);
    if (_linesPerPage < 1) _linesPerPage = 1;
    _maxLineWidth = usableWidth;

    Serial.printf("Layout: font=%d, fontH=%d, lineSpacing=%d, linesPerPage=%d, maxWidth=%d\n",
                  s.fontSize, bodyFontH, lineSpacing, _linesPerPage, _maxLineWidth);
}

void BookReader::closeBook() {
    updateReadingTime();
    saveProgress();
    _parser.close();
    _title = "";
    _filepath = "";
    _currentChapter = 0;
    _currentPage = 0;
    _totalPages = 0;
    _totalLines = 0;
    _lineOffsets.clear();
    _pages.clear();
    _currentPageLines.clear();
    _bookmarks.clear();
    _lineCachePath = "";
    _totalReadingTimeSec = 0;
    _totalPagesRead = 0;
    _sessionStartMs = 0;
    _lastTimeUpdateMs = 0;
}

void BookReader::loadChapter(int chapter) {
    if (chapter < 0 || chapter >= _parser.getChapterCount()) return;

    _currentChapter = chapter;
    _lineOffsets.clear();
    _pages.clear();
    _currentPageLines.clear();
    _totalLines = 0;

    Serial.printf("Chapter %d: loading (heap: %d, psram: %d)\n",
                  chapter, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
    yield();

    String text = _parser.getChapterText(chapter);
    Serial.printf("Chapter %d: text %d chars (heap: %d)\n",
                  chapter, (int)text.length(), (int)ESP.getFreeHeap());

    if (text.length() == 0) {
        Serial.printf("Chapter %d: empty text\n", chapter);
        text = "[Could not load chapter text]\n\nThe chapter may be too large.";
    }

    wrapTextToFile(text);
    text = String();  // free source text

    Serial.printf("Chapter %d: %d lines on SD (heap: %d)\n",
                  chapter, _totalLines, (int)ESP.getFreeHeap());

    paginateLines();
    _totalPages = _pages.size();
    if (_totalPages == 0) _totalPages = 1;
    if (_currentPage >= _totalPages) _currentPage = 0;
    updatePageLines();
}

void BookReader::updatePageLines() {
    _currentPageLines.clear();
    if (_currentPage < (int)_pages.size()) {
        const PageRange& pr = _pages[_currentPage];
        Serial.printf("updatePageLines: page %d, lines %d-%d from %s\n",
                      _currentPage, pr.lineStart, pr.lineEnd, _lineCachePath.c_str());

        // Read all lines for this page in a single file open
        File f = SD.open(_lineCachePath, FILE_READ);
        if (f) {
            for (int i = pr.lineStart; i < pr.lineEnd && i < _totalLines; i++) {
                if (i < (int)_lineOffsets.size()) {
                    f.seek(_lineOffsets[i]);
                    String line = f.readStringUntil('\n');
                    _currentPageLines.push_back(line);
                }
            }
            f.close();
            Serial.printf("updatePageLines: read %d lines (heap: %d)\n",
                          (int)_currentPageLines.size(), (int)ESP.getFreeHeap());
        } else {
            Serial.printf("updatePageLines: FAILED to open %s\n", _lineCachePath.c_str());
        }
    }

    if (_currentPageLines.empty()) {
        _currentPageLines.push_back("[No readable text in this section]");
    }
}

String BookReader::readLineFromCache(int lineIndex) {
    if (lineIndex < 0 || lineIndex >= _totalLines) return "";
    if (lineIndex >= (int)_lineOffsets.size()) return "";

    File f = SD.open(_lineCachePath, FILE_READ);
    if (!f) {
        Serial.printf("readLineFromCache: cannot open %s\n", _lineCachePath.c_str());
        return "[SD read error]";
    }

    f.seek(_lineOffsets[lineIndex]);
    String line = f.readStringUntil('\n');
    f.close();
    return line;
}

void BookReader::paginateLines() {
    _pages.clear();
    // Simple fixed-size pagination — no SD reads needed.
    // Image-aware page breaking would require reading every line from SD
    // which is too slow (503 file operations). Images that span a page
    // boundary will just be clipped, which is acceptable.
    int i = 0;
    while (i < _totalLines) {
        PageRange pr;
        pr.lineStart = i;
        pr.lineEnd = min(i + _linesPerPage, _totalLines);
        _pages.push_back(pr);
        i = pr.lineEnd;
    }
    _totalPages = _pages.size();
    if (_totalPages == 0) _totalPages = 1;
    Serial.printf("Paginated: %d pages from %d lines (%d lines/page)\n",
                  _totalPages, _totalLines, _linesPerPage);
}

// Write a single line to the cache file, tracking its offset
static void writeLine(File& f, std::vector<uint32_t>& offsets, const String& line) {
    offsets.push_back((uint32_t)f.position());
    f.print(line);
    f.print('\n');
}

void BookReader::wrapTextToFile(const String& text) {
    // Ensure cache directory exists
    if (!SD.exists(LINE_CACHE_DIR)) SD.mkdir(LINE_CACHE_DIR);

    _lineCachePath = String(LINE_CACHE_DIR) + "/ch" + String(_currentChapter) + ".txt";
    _lineOffsets.clear();
    _totalLines = 0;

    File f = SD.open(_lineCachePath, FILE_WRITE);
    if (!f) {
        Serial.println("ERROR: cannot open line cache for writing");
        return;
    }

    String workingText = text;
    if ((int)workingText.length() > MAX_WRAP_TEXT_CHARS) {
        workingText = workingText.substring(0, MAX_WRAP_TEXT_CHARS);
        workingText += "\n\n[Section truncated for device stability]";
    }

    int textLen = workingText.length();
    if (textLen == 0) { f.close(); return; }

    int start = 0;
    unsigned long lastYieldMs = millis();
    while (start < textLen) {
        unsigned long nowMs = millis();
        if (nowMs - lastYieldMs >= 50) { yield(); lastYieldMs = nowMs; }

        int nl = workingText.indexOf('\n', start);
        if (nl < 0) nl = textLen;

        String paragraph = workingText.substring(start, nl);
        paragraph.trim();

        if (paragraph.length() == 0) {
            writeLine(f, _lineOffsets, "");
            _totalLines++;
            start = nl + 1;
            continue;
        }

        // Handle image markers
        if (paragraph[0] == IMG_MARKER_BYTE) {
            String imgPath;
            if (inline_image_parse_raw(paragraph, imgPath)) {
                bool probed = false;
                if ((int)ESP.getFreeHeap() > 30000) {
                    int lineH = display_font_height() + _lineSpacing;
                    int maxImgH = _linesPerPage * lineH;
                    InlineImageInfo info;
                    if (inline_image_probe(_parser, imgPath, _maxLineWidth, maxImgH, info)) {
                        info.linesConsumed = max(1, (info.displayH + lineH - 1) / lineH);
                        writeLine(f, _lineOffsets, inline_image_build_marker(
                            info.zipPath, info.displayW, info.displayH, info.linesConsumed));
                        _totalLines++;
                        for (int j = 1; j < info.linesConsumed; j++) {
                            writeLine(f, _lineOffsets, IMG_CONT_MARKER);
                            _totalLines++;
                        }
                        probed = true;
                    }
                }
                if (!probed) {
                    writeLine(f, _lineOffsets, "[Image]");
                    _totalLines++;
                }
                start = nl + 1;
                continue;
            }
        }

        // Word-wrap the paragraph
        String currentLine;
        int currentWidth = 0;
        int spaceWidth = display_text_width(" ");
        int indentWidth = spaceWidth * 3;
        bool firstLine = true;

        int wStart = 0;
        int paraLen = paragraph.length();
        while (wStart < paraLen) {
            while (wStart < paraLen && paragraph[wStart] == ' ') wStart++;
            if (wStart >= paraLen) break;

            int wEnd = wStart;
            while (wEnd < paraLen && paragraph[wEnd] != ' ') wEnd++;

            String word = paragraph.substring(wStart, wEnd);
            int wordWidth = display_text_width(word.c_str());

            if (currentLine.length() == 0) {
                if (firstLine) { currentLine = "   "; currentWidth = indentWidth; }
                if (currentWidth + wordWidth > _maxLineWidth) {
                    String partial = currentLine;
                    int pw = currentWidth;
                    for (int ci = 0; ci < (int)word.length(); ci++) {
                        char ch[2] = { word[ci], 0 };
                        int cw = display_text_width(ch);
                        if (pw + cw > _maxLineWidth && partial.length() > 0) {
                            writeLine(f, _lineOffsets, partial);
                            _totalLines++;
                            firstLine = false;
                            partial = "";
                            pw = 0;
                        }
                        partial += word[ci];
                        pw += cw;
                    }
                    if (partial.length() > 0) { currentLine = partial; currentWidth = pw; }
                } else {
                    currentLine += word;
                    currentWidth += wordWidth;
                    firstLine = false;
                }
            } else if (currentWidth + spaceWidth + wordWidth <= _maxLineWidth) {
                currentLine += " " + word;
                currentWidth += spaceWidth + wordWidth;
            } else {
                writeLine(f, _lineOffsets, currentLine);
                _totalLines++;
                firstLine = false;
                currentLine = word;
                currentWidth = wordWidth;
            }
            wStart = wEnd;
        }

        if (currentLine.length() > 0) {
            writeLine(f, _lineOffsets, currentLine);
            _totalLines++;
        }

        start = nl + 1;
    }

    f.close();
}

bool BookReader::nextPage() {
    bool chapterChange = false;
    if (_currentPage + 1 < _totalPages) {
        _currentPage++;
    } else if (_currentChapter + 1 < _parser.getChapterCount()) {
        _currentPage = 0;
        chapterChange = true;
        Serial.printf("nextPage: advancing to chapter %d (heap free: %d, PSRAM free: %d)\n",
                      _currentChapter + 1, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
        loadChapter(_currentChapter + 1);
    } else {
        return false;
    }

    _totalPagesRead++;
    _chapterChanged = chapterChange;
    updatePageLines();
    if (++_pageTurnsSinceSave >= SAVE_EVERY_N_TURNS) {
        updateReadingTime();
        saveProgress();
        _pageTurnsSinceSave = 0;
    }
    return true;
}

bool BookReader::prevPage() {
    bool chapterChange = false;
    if (_currentPage > 0) {
        _currentPage--;
    } else if (_currentChapter > 0) {
        chapterChange = true;
        Serial.printf("prevPage: going back to chapter %d (heap free: %d, PSRAM free: %d)\n",
                      _currentChapter - 1, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
        loadChapter(_currentChapter - 1);
        _currentPage = _totalPages - 1;
    } else {
        return false;
    }

    _totalPagesRead++;
    _chapterChanged = chapterChange;
    updatePageLines();
    if (++_pageTurnsSinceSave >= SAVE_EVERY_N_TURNS) {
        updateReadingTime();
        saveProgress();
        _pageTurnsSinceSave = 0;
    }
    return true;
}

void BookReader::updateReadingTime() {
    unsigned long now = millis();
    if (_lastTimeUpdateMs > 0 && now > _lastTimeUpdateMs) {
        unsigned long elapsedMs = now - _lastTimeUpdateMs;
        // Cap at 5 minutes per interval to avoid counting idle/sleep time
        if (elapsedMs > 300000UL) elapsedMs = 300000UL;
        _totalReadingTimeSec += elapsedMs / 1000;
    }
    _lastTimeUpdateMs = now;
}

void BookReader::jumpToChapter(int chapter) {
    if (chapter < 0 || chapter >= _parser.getChapterCount()) return;
    _currentPage = 0;
    _currentPageLines.clear();
    loadChapter(chapter);
}

void BookReader::restorePage(int page) {
    if (page < 0) page = 0;
    if (page >= _totalPages) page = _totalPages - 1;
    _currentPage = page;
    updatePageLines();
}

const std::vector<String>& BookReader::getPageLines() const {
    return _currentPageLines;
}

String BookReader::getChapterTitle(int index) {
    return _parser.getChapterTitle(index);
}

String BookReader::getTocLabel(int index) {
    return _parser.getTocLabel(index);
}

int BookReader::getTocChapterIndex(int index) {
    return _parser.getTocChapterIndex(index);
}

// ─── Bookmarks ──────────────────────────────────────────────────────

void BookReader::addBookmark() {
    // Don't add duplicate
    for (const auto& bm : _bookmarks) {
        if (bm.chapter == _currentChapter && bm.page == _currentPage) return;
    }
    Bookmark bm;
    bm.chapter = _currentChapter;
    bm.page = _currentPage;
    String chapterTitle = _parser.getChapterTitle(_currentChapter);
    chapterTitle.trim();
    if (chapterTitle.length() > 24) {
        chapterTitle = chapterTitle.substring(0, 21) + "...";
    }
    bm.label = String("Ch ") + String(_currentChapter + 1) + ", Pg " + String(_currentPage + 1);
    if (chapterTitle.length() > 0) {
        bm.label += " - ";
        bm.label += chapterTitle;
    }
    _bookmarks.push_back(bm);
    saveProgress();
    Serial.printf("Bookmark added: %s\n", bm.label.c_str());
}

void BookReader::removeBookmark(int idx) {
    if (idx >= 0 && idx < (int)_bookmarks.size()) {
        _bookmarks.erase(_bookmarks.begin() + idx);
        saveProgress();
    }
}

bool BookReader::jumpToBookmark(int idx) {
    if (idx < 0 || idx >= (int)_bookmarks.size()) return false;
    const Bookmark& bm = _bookmarks[idx];
    _currentPage = bm.page;
    loadChapter(bm.chapter);
    _currentPage = bm.page;  // loadChapter may reset page, restore it
    if (_currentPage >= _totalPages) _currentPage = _totalPages - 1;
    updatePageLines();
    return true;
}

bool BookReader::isCurrentPageBookmarked() const {
    for (const auto& bm : _bookmarks) {
        if (bm.chapter == _currentChapter && bm.page == _currentPage) return true;
    }
    return false;
}

// ─── Progress save/load ────────────────────────────────────────────

static String progressPath(const String& filepath) {
    String name = filepath;
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    return String(PROGRESS_DIR) + "/" + name + ".json";
}

static String progressTmpPath(const String& filepath) {
    String name = filepath;
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    return String(PROGRESS_DIR) + "/." + name + ".tmp";
}

void BookReader::saveProgress() {
    if (_filepath.length() == 0) return;

    String path = progressPath(_filepath);
    String tmpPath = progressTmpPath(_filepath);

    int chapterCount = _parser.getChapterCount();
    // Allocate enough for base fields + bookmarks + chapter title cache
    // (up to 200 chapters × ~64 bytes each + 1024 base)
    size_t docSize = 2048;
    DynamicJsonDocument doc(docSize);
    doc["chapter"] = _currentChapter;
    doc["page"] = _currentPage;
    doc["total_chapters"] = chapterCount;
    doc["last_read"] = _lastReadOrder;

    // Reading statistics
    doc["reading_time_sec"] = _totalReadingTimeSec;
    doc["pages_read"] = _totalPagesRead;

    // Cache invalidation: record EPUB file size so stale cache is discarded
    // if the file is replaced.
    doc["cache_version"] = 1;
    File ef = SD.open(_filepath.c_str(), FILE_READ);
    if (ef) {
        doc["epub_size"] = (uint32_t)ef.size();
        ef.close();
    }

    // Chapter title cache persistence removed to save heap — titles are
    // loaded lazily from ZIP when the TOC screen is opened.

    // Save bookmarks
    if (!_bookmarks.empty()) {
        JsonArray bArr = doc.createNestedArray("bookmarks");
        for (const auto& bm : _bookmarks) {
            JsonObject obj = bArr.createNestedObject();
            obj["chapter"] = bm.chapter;
            obj["page"] = bm.page;
            obj["label"] = bm.label;
        }
    }

    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(path, tmpPath, json)) {
        Serial.printf("Progress saved atomically: ch%d pg%d -> %s\n",
                      _currentChapter, _currentPage, path.c_str());
    } else {
        Serial.printf("Progress save failed: %s\n", path.c_str());
    }
}

void BookReader::loadProgress() {
    if (_filepath.length() == 0) return;

    String path = progressPath(_filepath);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        _currentChapter = 0;
        _currentPage = 0;
        return;
    }

    int chapterCount = _parser.getChapterCount();
    size_t docSize = 2048;
    DynamicJsonDocument doc(docSize);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        _currentChapter = 0;
        _currentPage = 0;
        return;
    }

    _currentChapter = doc["chapter"] | 0;
    _currentPage = doc["page"] | 0;
    _lastReadOrder = doc["last_read"] | (uint32_t)0;
    _totalReadingTimeSec = doc["reading_time_sec"] | (uint32_t)0;
    _totalPagesRead = doc["pages_read"] | (uint32_t)0;

    if (_currentChapter >= chapterCount) {
        _currentChapter = 0;
        _currentPage = 0;
    }

    // Chapter title cache restore disabled — the 72+ String objects consumed
    // ~60KB of heap on books with many spine entries, leaving <11KB for chapter
    // loading which caused OOM crashes.  Titles are loaded lazily from ZIP when
    // the TOC screen is opened instead.
    Serial.printf("Progress loaded (heap: %d)\n", (int)ESP.getFreeHeap());

    // Load bookmarks
    _bookmarks.clear();
    if (doc.containsKey("bookmarks")) {
        JsonArray bArr = doc["bookmarks"].as<JsonArray>();
        for (JsonObject obj : bArr) {
            Bookmark bm;
            bm.chapter = obj["chapter"] | 0;
            bm.page = obj["page"] | 0;
            bm.label = obj["label"].as<String>();
            if (bm.label.length() == 0) {
                bm.label = String("Ch") + String(bm.chapter + 1) + " Pg" + String(bm.page + 1);
            }
            _bookmarks.push_back(bm);
        }
    }

    Serial.printf("Progress loaded: ch%d pg%d, %d bookmarks\n",
                  _currentChapter, _currentPage, _bookmarks.size());
}
