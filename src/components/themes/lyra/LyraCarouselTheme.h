#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra Carousel metrics ported for Casper on CrossPoint 1.5.
// Full CrossInk carousel rendering is deferred; this theme uses Lyra drawing
// with carousel-oriented home metrics so it can be selected without dragging
// in CrossInk-only BaseTheme APIs (const char* menus, getCoverThumbPath WxH, etc.).
namespace LyraCarouselMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  v.listRowHeight = 35;
  v.menuRowHeight = 64;
  v.menuSpacing = 8;
  v.homeTopPadding = 28;
  v.homeCoverHeight = 600;
  v.homeCoverTileHeight = 660;
  v.homeRecentBooksCount = 3;
  v.keyboardKeyHeight = 50;
  v.keyboardCenteredText = true;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace LyraCarouselMetrics

class LyraCarouselTheme : public LyraTheme {
 public:
  // Thin stub: Lyra drawing + carousel metrics only.
};
