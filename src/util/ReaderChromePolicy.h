#pragma once

#include <algorithm>

// Reader bottom reserve. Portrait dictionary/clip hints overlay the status
// lane at the panel edge. On X4, stacking screenMargin + line clearance on
// top of that strip left a 1–2 line hole after the X3 48→34 hint shrink.
namespace readerchrome {

// Portrait front-label strip. Same theme height on X3 and X4 (Bare = 34).
inline int portraitHintStrip(const int themeHintHeight, const bool /*x4*/) { return std::max(1, themeHintHeight); }

// Leftover px above/below a footer label centered in the band.
inline int portraitFooterLabelAir(const int bandH, const int lineH) {
  return std::max(0, (std::max(1, bandH) - std::max(1, lineH)) / 2);
}

// Extra px between the 34px label band and the panel edge.
// X3 stays flush. On X4, 0–9px still nicked the UI_10 line on the lip;
// 56px cleared it but sat on Penumbra Recents (layoutH ate the list).
// 20px is a small bezel gap (~2mm), not a second footer. Reader overlay
// stays on the 34px strip.
constexpr int kX4PortraitFooterEdgePad = 20;

inline int portraitFooterEdgePad(const bool x4) { return x4 ? kX4PortraitFooterEdgePad : 0; }

inline int portraitFooterLayoutH(const int themeHintHeight, const bool x4) {
  return portraitHintStrip(themeHintHeight, x4) + portraitFooterEdgePad(x4);
}

// Top of the 34px label band. inverted → pad at the logical top.
inline int portraitFooterBarY(const int pageH, const int barH, const int edgePad, const bool inverted) {
  const int pad = std::max(0, edgePad);
  const int h = std::max(1, barH);
  if (inverted) return pad;
  return std::max(0, pageH - pad - h);
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
