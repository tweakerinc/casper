#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

// UI display policy for home / settings / menus (not reader page turns).
//
// Reader Anti-Ghosting (SETTINGS.refreshFrequency) is applied only by
// ReaderUtils::displayWithRefreshCycle — same X3 soft bank as displaySoftReinforce.
//
// Why Library looked clean while Home→Menu showed the full home image
// -------------------------------------------------------------------
// After cover multipass, controller DTM1/DTM2 are rebased to the *home* BW
// shell (glass may still show greys). Library paints dense ink and FASTs
// against that home baseline → strong differential, clean plate.
// Menu used to call cleanupGrayscale with the *new* sparse FB *after* draw:
// both planes became the menu with no glass update, so FAST saw zero diff and
// the main screen stayed visible until many Up/Down FASTs eroded residual.
// Fix: never cleanup with the destination plate before its first display;
// open with FAST (+ optional soft settle). Cursor moves: plain FAST only.

namespace UiGhostPolicy {

namespace detail {
inline bool& hardScrubArmed() {
  static bool armed = false;
  return armed;
}
inline bool& greyscaleOnPanel() {
  static bool greys = false;
  return greys;
}
}  // namespace detail

inline void noteHalf() {
  detail::hardScrubArmed() = false;
  detail::greyscaleOnPanel() = false;
}

inline void requestHardScrub() { detail::hardScrubArmed() = true; }

inline void clearHardScrub() { detail::hardScrubArmed() = false; }

inline bool hardScrubArmed() { return detail::hardScrubArmed(); }

// Clock AA / reader AA leave greyscale on glass while FB is restored BW.
// QR sleep FAST then diffs the two planes into a black/messed frame
// (device v50: Home clock_aa then SLEEP).
inline void noteGreyscaleOnPanel() { detail::greyscaleOnPanel() = true; }
inline bool panelHoldsGreyscale() { return detail::greyscaleOnPanel(); }

// Hard clean — X3 HALF + resync. Force Refresh / intentional home scrub only.
inline void displayHalf(const GfxRenderer& renderer) {
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  noteHalf();
}

// X3 soft B/W reinforce only (OEM AA-pre-BW mid). No strong plate first.
// Prefer displaySoftOpen for full-screen swaps over home.
inline void displaySoftReinforce(const GfxRenderer& renderer) {
  if (gpio.deviceIsX3()) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// New plate open (menu, library, settings, recents):
//   1) Full FAST — strong differential so sparse white menus actually clear home
//   2) softCount soft pulls — gentle residual settle (X3; capped)
// Cursor / band nav must use displayMenuFrame or displayMenuBand instead.
inline void displaySoftOpen(const GfxRenderer& renderer, int softCount = 1) {
  clearHardScrub();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  if (!gpio.deviceIsX3() || softCount < 1) return;
  if (softCount > 2) softCount = 2;
  for (int i = 0; i < softCount; ++i) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  }
}

// Ongoing UI paints (list cursor, re-draw): plain FAST unless hard scrub armed.
// Not soft — avoids progressive erase while the user scrolls.
inline void displayMenuFrame(const GfxRenderer& renderer) {
  if (detail::hardScrubArmed()) {
    displayHalf(renderer);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// First full-frame open (library, recents, etc.).
inline void displayFastFull(const GfxRenderer& renderer) { displaySoftOpen(renderer, /*softCount=*/1); }

// Menu cursor / band: always plain FAST. Never soft, never HALF.
inline void displayMenuBand(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

inline void displayListNav(const GfxRenderer& renderer, int listTopY) {
  const int h = renderer.getScreenHeight() - listTopY;
  if (h <= 0) {
    displayMenuFrame(renderer);
    return;
  }
  displayMenuBand(renderer, 0, listTopY, renderer.getScreenWidth(), h);
}

inline bool sameListPage(int indexA, int indexB, int pageItems) {
  if (pageItems <= 0) return false;
  if (indexA < 0 || indexB < 0) return false;
  return (indexA / pageItems) == (indexB / pageItems);
}

inline void displayHomeUnderUpdate(const GfxRenderer& renderer, int winX, int winY, int winW, int winH) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(winX, winY, winW, winH);
  }
}

inline void displaySoftFull(const GfxRenderer& renderer) { displayMenuFrame(renderer); }

inline void displayPartialOrSoft(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

}  // namespace UiGhostPolicy
