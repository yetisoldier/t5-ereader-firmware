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
    String assetPath;     // extracted cached image path on SD
    int    displayW = 0;  // scaled width for display (pixels)
    int    displayH = 0;  // scaled height for display (pixels)
    int    linesConsumed = 0; // how many text-line slots this image occupies
};

// Check whether a wrapped-line string is an image marker.
bool inline_image_is_marker(const String& line);

// Check whether a wrapped-line string is an image continuation placeholder.
bool inline_image_is_continuation(const String& line);

// Parse an enriched marker "\x01IMG|assetPath|w|h|lines\x01" produced by wrapText.
// Returns true and fills out* on success.
bool inline_image_parse_enriched(const String& line, String& outPath,
                                 int& outW, int& outH, int& outLines);

// Parse a raw marker "\x01IMG|relativePath\x01" produced by stripHtml.
// Returns the relative image path (before resolution).
bool inline_image_parse_raw(const String& line, String& outPath);

// Build an enriched marker string for storage in wrappedLines.
String inline_image_build_marker(const String& assetPath, int w, int h, int lines);

// Extract an EPUB image to SD cache if needed, then probe dimensions without
// fully decoding it. Fills displayW/displayH scaled to fit within maxW x maxH
// (aspect-ratio preserved). Returns false if the asset is missing,
// unsupported, or corrupt.
bool inline_image_probe(EpubParser& parser, const String& bookPath, const String& zipPath,
                        int maxW, int maxH, InlineImageInfo& out);

// Render an extracted cached image file to the portrait framebuffer.
// Decodes via JPEGDEC/PNGdec file callbacks directly into the display buffer.
bool inline_image_render(const String& assetPath,
                         int dstX, int dstY, int dstW, int dstH);
