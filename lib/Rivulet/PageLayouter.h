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
};

// Pixel gap for extra paragraph spacing (matches CasperSettings height enum).
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
