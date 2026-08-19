#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IrFormat.h"
#include "PageMap.h"

namespace rivulet {

// One painted fragment on a page (single style run segment).
struct GlyphSpan {
  int16_t x = 0;
  int16_t y = 0;  // line top
  int fontId = 0;
  uint8_t epdStyle = 0;   // EpdFontFamily bits (may include DROP_CAP)
  uint8_t dropScale = 0;  // 0 = normal; 2–4 = NN drop-cap scale
  std::string text;       // owned UTF-8 fragment for this span
};

// Raster plate (JPEG/PNG) laid out on the page — painted via ImageBlock.
struct ImagePlate {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  std::string href;  // EPUB package-relative path
};

// A drawn hairline (thematic break / <hr>). Painting this as a real rule instead
// of a row of em-dash glyphs makes section breaks look typeset rather than typed,
// and removes the dependency on the active font actually having U+2014.
struct RulePlate {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 1;
};

struct LaidOutPage {
  std::vector<GlyphSpan> spans;
  std::vector<ImagePlate> images;
  std::vector<RulePlate> rules;
  IrCursor start{};
  IrCursor end{};     // exclusive end cursor (start of next page)
  int16_t contentH = 0;
  bool atChapterEnd = false;

  // Drop-cap exclusion for wrap (page-local).
  int16_t dropZoneW = 0;
  int16_t dropZoneH = 0;
  bool hasDropZone = false;

  void clear() {
    spans.clear();
    images.clear();
    rules.clear();
    start = {};
    end = {};
    contentH = 0;
    atChapterEnd = false;
    dropZoneW = 0;
    dropZoneH = 0;
    hasDropZone = false;
  }

  // Classic section.bin idea, Rivulet data: persist a fully laid-out page so
  // resume / revisit is deserialize + paint instead of re-layout. Key mismatch
  // (font, margins, viewport) → load fails and caller re-layouts.
  bool saveToFile(const char* path, const RenderKey& key, int pageIndex) const;
  bool loadFromFile(const char* path, const RenderKey& expectedKey, int expectedPage);
};

}  // namespace rivulet
