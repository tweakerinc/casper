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
