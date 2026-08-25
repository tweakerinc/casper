#pragma once

#include <algorithm>

// Reader bottom reserve. Portrait dictionary/clip hints overlay the status
// lane at the panel edge. On X4, stacking screenMargin + line clearance on
// top of that strip left a 1–2 line hole after the X3 48→34 hint shrink.
namespace readerchrome {

// Text-only UI_10 labels (~17px). 34px left empty pad the reader reserved
// on every page so Dictionary never covered text.
constexpr int kX4PortraitHintMax = 24;

inline int portraitHintStrip(const int themeHintHeight, const bool x4) {
  if (!x4) return themeHintHeight;
  return std::min(themeHintHeight, kX4PortraitHintMax);
}

struct BottomIn {
  int oBottom = 0;
  int screenMargin = 0;
  int statusBarHeight = 0;
  int progressBarHeight = 0;
  int statusBarVerticalMargin = 0;
  int clearance = 0;
  int hintStrip = 0;  // 0 in landscape (side strip is a horizontal margin)
  bool x4 = false;
};

inline int chromeBand(const BottomIn& in) {
  int band = in.statusBarHeight + in.clearance;
  if (in.statusBarHeight == 0 || in.statusBarHeight == in.progressBarHeight) {
    band = std::max(band, in.statusBarHeight + in.statusBarVerticalMargin + in.clearance);
  }
  if (in.hintStrip > 0) {
    band = std::max(band, in.hintStrip);
  }
  return band;
}

inline int marginBottom(const BottomIn& in) {
  if (in.x4 && in.hintStrip > 0) {
    // Overlay: status lane and hint strip share the panel edge. Do not add
    // screenMargin or a third of a body line on top of that max.
    int band = std::max(in.statusBarHeight, in.hintStrip);
    if (in.statusBarHeight == 0 || in.statusBarHeight == in.progressBarHeight) {
      band = std::max(band, in.statusBarHeight + in.statusBarVerticalMargin);
    }
    return std::max(0, in.oBottom + band);
  }
  return std::max(0, in.oBottom + in.screenMargin + chromeBand(in));
}

}  // namespace readerchrome
