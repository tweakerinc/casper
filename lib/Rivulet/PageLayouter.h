#pragma once

#include <algorithm>
#include <cstdint>

#include "ChapterIr.h"
#include "IrFormat.h"
#include "LaidOutPage.h"
#include "PageMap.h"

class GfxRenderer;

namespace rivulet {

struct LayoutParams {
  RenderKey key{};
  float lineCompression = 1.0f;
  int bodyEmPx = 14;  // ascender-ish for indent math
  // Extra gap after every paragraph (settings → extra paragraph spacing).
  bool extraParagraphSpacing = false;
  // When extraParagraphSpacing: 0 = ½ line (default), 1 = full line, 2 = ¼ line.
  uint8_t extraParagraphSpacingHeight = 0;
  // Idle map abort: peek GPIO without latching edges. Null = never abort.
  // Paint passes must leave this null so a tap cannot cancel the page on glass.
  bool (*shouldAbort)() = nullptr;
  // Pagination-only pass: compute the page's end cursor and geometry but do not
  // emit paintable text spans.
  //
  // Page-map work (opening at a resume page, extendPageMap, the idle tick,
  // goToLastPage) only ever reads `out.end` / `atChapterEnd` — it throws the
  // spans away. Emitting them anyway meant one GlyphSpan per word (~48 B, each
  // owning a std::string copy of the word) plus the vector to hold them, for
  // every page walked. Opening a book at page 30 of a chapter did that thirty
  // times before the first pixel.
  //
  // Line breaking, justification, drop-cap zones, image placement and every
  // y-advance still run exactly as in a painting pass, so the end cursor this
  // produces is identical — only the throwaway output is skipped.
  bool measureOnly = false;
};

// Pixel gap for extra paragraph spacing (matches CrossPointSettings height enum).
inline int extraParaGapPx(int bodyLine, uint8_t height) {
  if (bodyLine <= 0) return 1;
  switch (height) {
    case 2:  // quarter
      return std::max(1, bodyLine / 4);
    case 1:  // full
      return bodyLine;
    default:  // half
      return std::max(1, bodyLine / 2);
  }
}

// Tier A: lay out exactly one page starting at `from`, into `out`.
// Does not allocate whole-chapter geometry.
class PageLayouter {
 public:
  // Returns false if chapter empty or from past end.
  static bool layoutPage(const ChapterIr& chapter, const GfxRenderer& renderer, const LayoutParams& params,
                         const IrCursor& from, LaidOutPage& out);

  // Walk entire chapter, fill PageMap with every page start + mark complete.
  // Uses same layout rules as layoutPage (slower; call idle / after open).
  static bool buildFullPageMap(const ChapterIr& chapter, const GfxRenderer& renderer, const LayoutParams& params,
                               PageMap& map);
};

}  // namespace rivulet
