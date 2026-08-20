#pragma once

#include <cstdint>

#include "components/themes/lyra/LyraTheme.h"

// Thin Minimal theme for Casper 1.5: metrics + text-only bottom labels.
// Dashboard (Stats) and Bare both use this footer style; Bare only reorders labels.
namespace MinimalMetrics {
constexpr int coverWidthForHeight(const int coverHeight) {
  return static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
}

constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 583;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  // Text-only footer (no rounded chrome): one UI_10 line centred in the band.
  // 48 left ~31px of empty padding, which the reader had to reserve on every
  // page so Dictionary/Clip could never cover text. 34 still clears the label
  // with air above and below, and hands the difference back to body text.
  v.buttonHintsHeight = 34;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverWidth = coverWidthForHeight(values.homeCoverHeight);
constexpr int homeCoverImageWidth = homeCoverWidth;
constexpr int homeCoverImageHeight = 525;
}  // namespace MinimalMetrics

class MinimalTheme : public LyraTheme {
 public:
  // Text-only footer (four equal columns). No rounded button outlines.
  // Label *order* is chosen by the activity (Stats vs Bare).
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
};
