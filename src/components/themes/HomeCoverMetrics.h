#pragma once

#include <algorithm>

// Home hero cover generation for Bare / Stats / Stats-Life.
//
// Epub::generateThumbBmp(H) writes floor(H*3/4)×H (3:4 contain-fit, c30 balanced
// Atkinson — Casper v0.1.3). Same JpegToBmpConverter for all themes. Nearest-
// neighbor scale of that 2-bit dither = gridlines / bands — gen size must match plate.
//   Bare           → fixed 420×560
//   Stats+Stats-Life → ONE shared height key (same thumb file for both themes)
//
// LAW: plate width/height for blit == thumbWidthForHeight(H)×H. Never shrink the
// plate after gen (that forces scale-down and invents hairlines).
namespace HomeCoverMetrics {
// Epub gen width for a given height key.
constexpr int thumbWidthForHeight(const int height) {
  return height > 0 ? (height * 3 + 1) / 4 : 1;
}

// Bare-native (do not enlarge — Bare quality depends on 1:1).
constexpr int bareImageWidth = 420;
constexpr int bareImageHeight = 560;
constexpr int imageWidth = bareImageWidth;
constexpr int imageHeight = bareImageHeight;
constexpr int thumbHeight = bareImageHeight;
// Compact shelf thumbs (legacy Dashboard helper paths still reference this size).
constexpr int homeShelfThumbHeight = 168;

// Height key so gen width == maxCoverW (3:4 plate fills the column).
inline int thumbHeightForCoverWidth(const int maxCoverW) {
  const int w = std::max(80, maxCoverW);
  // W = (H*3+1)/4  →  H ≈ (4*W)/3
  return std::max(200, (w * 4) / 3);
}

// Stats + Stats-Life layout: [kEdgeGap][cover][≥kEdgeGap][stats][kEdgeGap].
constexpr int kFocusEdgeGap = 8;
// Stats column budget (right side). Larger budget = narrower/shorter cover gen.
// 150 leaves room for Source Serif labels while maximizing the jacket plate.
// If labels ever overflow, truncate — never shrink the plate after gen.
constexpr int kFocusStatsColBudget = 150;
// Fixed under-box reserve so Stats and Stats-Life share the same cover plate
// (title/author vs lifetime card must not change gen height).
constexpr int kStatsFamilyUnderBoxReserve = 100;
constexpr int kStatsFamilyChromeFooter = 50 + 48 + 40;  // chrome + menu + pads

inline int focusMaxCoverWidth(const int pageW) {
  return std::max(120, pageW - kFocusStatsColBudget - 3 * kFocusEdgeGap);
}

// Shared Stats/Stats-Life hero height — use for BOTH gen and layout plate.
// Depends only on page size, not which under-box content is drawn.
inline int statsFamilyHeroThumbHeight(const int pageW, const int pageH) {
  const int hFromW = thumbHeightForCoverWidth(focusMaxCoverWidth(pageW));
  const int maxBandH =
      std::max(200, pageH - kStatsFamilyChromeFooter - kStatsFamilyUnderBoxReserve);
  return std::max(200, std::min(hFromW, maxBandH));
}

// Exact plate size for the shared Stats-family thumb (1:1 blit).
inline void statsFamilyHeroPlate(const int pageW, const int pageH, int& coverW, int& coverH) {
  coverH = statsFamilyHeroThumbHeight(pageW, pageH);
  coverW = thumbWidthForHeight(coverH);
}

// Legacy name used by FocusTheme sizeCoverFrame (maxCoverBandH from layout).
inline int focusHeroThumbHeight(const int pageW, const int maxCoverBandH) {
  const int hFromW = thumbHeightForCoverWidth(focusMaxCoverWidth(pageW));
  return std::max(200, std::min(hFromW, std::max(200, maxCoverBandH)));
}

// Deprecated Dashboard sizing (Stats-Life no longer uses Dashboard home).
inline int dashboardHeroThumbHeight(const int pageW, const int maxArtH) {
  return statsFamilyHeroThumbHeight(pageW, maxArtH + kStatsFamilyChromeFooter + kStatsFamilyUnderBoxReserve);
}
}  // namespace HomeCoverMetrics
