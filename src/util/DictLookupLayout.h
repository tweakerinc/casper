#pragma once

#include <algorithm>

// Dictionary Lookup / Clip overlay chrome.
//
// After reader top-chrome reclaim, marginTop can be shorter than the title
// line. Centering in that band put "Dictionary Lookup" at y=2, inside the
// 9px viewable bezel, so the heading looked clipped on X4 (and X3).
namespace dictlookup {

constexpr int kModeTitleAir = 4;
constexpr int kTitleToCardGap = 8;

struct HighlightRect {
  int x;
  int y;
  int w;
  int h;
};

// Equal air above the em-box and below descenders (~1/8 of the ascender).
inline int wordHighlightAir(const int ascender) {
  const int asc = std::max(1, ascender);
  return std::max(1, (asc + 3) / 8);
}

inline int wordHighlightPadX(const int linePitch) { return std::max(1, std::min(2, std::max(1, linePitch) / 16)); }

inline int wordHighlightDescAbs(const int descender) { return descender < 0 ? -descender : descender; }

// drawText y is the em-box top; baseline = wordY + ascender. Center the black
// bar on that ink span so it covers a little above and below the letters at
// every size. Do not stretch to the next row (that is leading, not ink).
// nextRowY <= wordY means last row (no next-line ceiling).
inline HighlightRect wordHighlightRect(const int wordX, const int wordY, const int wordWidth, const int linePitch,
                                       const int ascender, const int descender, const int nextRowY) {
  const int padX = wordHighlightPadX(linePitch);
  const int asc = std::max(1, ascender);
  const int desc = wordHighlightDescAbs(descender);
  const int air = wordHighlightAir(asc);
  const int inkH = asc + desc;
  const int inkBottom = wordY + inkH;

  HighlightRect r;
  r.x = wordX - padX;
  r.w = std::max(0, wordWidth + padX * 2);

  const bool hasNext = nextRowY > wordY;
  const int maxBottom = hasNext ? nextRowY - 1 : inkBottom + air;
  const int belowRoom = maxBottom - inkBottom;

  int pad = air;
  if (belowRoom < air) {
    pad = std::max(0, belowRoom);
  }
  r.y = wordY - pad;
  r.h = inkH + pad * 2;
  if (hasNext && r.y + r.h > maxBottom) {
    r.h = maxBottom - r.y;
  }
  if (r.h < 0) {
    r.y = wordY;
    r.h = 0;
  }
  return r;
}

// 1-bit highlight (clippings / footnote refs). drawText y is em-box top;
// a LightGray dither rect here is a checkerboard box on X4, not a mark on the word.
inline int wordUnderlineY(const int wordY, const int ascender) { return wordY + std::max(6, ascender) + 2; }

inline int modeTitleY(const int viewableTop) { return std::max(0, viewableTop + kModeTitleAir); }

inline int modeTitleWipeH(const int titleY, const int lineH, const int marginTop) {
  const int fromGlyph = titleY + std::max(1, lineH) + 2;
  const int fromMargin = std::max(0, marginTop - 2);
  return std::max(fromGlyph, fromMargin);
}

// Definition card sits under the mode title. Vertical centering left a large
// hole above Back/Done on short lookups (X4 portrait especially).
inline int definitionTopReserve(const int viewableTop, const int titleLineH) {
  return modeTitleY(viewableTop) + std::max(1, titleLineH) + kTitleToCardGap;
}

}  // namespace dictlookup
