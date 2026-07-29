#pragma once

#include <cstdint>

#include "components/themes/lyra/LyraTheme.h"

// Thin Minimal theme for CrossPoint 1.5: metrics + text-only bottom labels.
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
  // Room for UI_12 text-only footer (no rounded chrome), same as Bare.
  v.buttonHintsHeight = 48;
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
