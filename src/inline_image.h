#pragma once

#include <Arduino.h>

// Forward declarations — avoid pulling in full epub.h
class EpubParser;

// Marker prefix byte used in wrapped lines to identify image placeholders.
// \x01 cannot appear in normal stripped text.
#define IMG_MARKER_BYTE '\x01'
#define IMG_MARKER_PREFIX "\x01IMG|"
#define IMG_CONT_MARKER  "\x01IMGCONT\x01"

struct InlineImageInfo {
    String zipPath;       // resolved absolute path within ZIP
    int    displayW = 0;  // scaled width for display (pixels)
    int    displayH = 0;  // scaled height for display (pixels)
    int    linesConsumed = 0; // how many text-line slots this image occupies
};

// Check whether a wrapped-line string is an image marker.
bool inline_image_is_marker(const String& line);

// Check whether a wrapped-line string is an image continuation placeholder.
bool inline_image_is_continuation(const String& line);

// Parse an enriched marker "\x01IMG|zipPath|w|h|lines\x01" produced by wrapText.
// Returns true and fills out* on success.
bool inline_image_parse_enriched(const String& line, String& outPath,
                                 int& outW, int& outH, int& outLines);

// Parse a raw marker "\x01IMG|relativePath\x01" produced by stripHtml.
// Returns the relative image path (before resolution).
bool inline_image_parse_raw(const String& line, String& outPath);

// Build an enriched marker string for storage in wrappedLines.
String inline_image_build_marker(const String& zipPath, int w, int h, int lines);

// Probe image dimensions from the EPUB ZIP without fully decoding.
// Loads compressed data into PSRAM, reads JPEG/PNG header, frees.
// Fills displayW/displayH scaled to fit within maxW x maxH (aspect-ratio preserved).
// Returns false if the asset is missing, unsupported, or corrupt.
bool inline_image_probe(EpubParser& parser, const String& zipPath,
                        int maxW, int maxH, InlineImageInfo& out);

// Render an image from the EPUB ZIP at the given portrait-framebuffer position.
// Loads compressed data into PSRAM, decodes via JPEGDEC/PNGdec scanline
// callbacks, writes pixels to the display framebuffer, frees data.
bool inline_image_render(EpubParser& parser, const String& zipPath,
                         int dstX, int dstY, int dstW, int dstH);
