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
// Fix: never cleanup with the destination plate before its first display.
// X3 opens FAST + mid-bank settle. X4 SoftOpen is one HALF (no mid bank);
// list cursor stays FAST. Do not window X4 UI — displayWindow only resyncs
// RED for the strip and ghosts the rest of the plate.

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
// Reader/home BW FAST replaced the greys. Sleep HALF against a stale flag is
// the black flash on QR (Penumbra home → book → sleep; glass is already BW).
inline void noteBwOnPanel() { detail::greyscaleOnPanel() = false; }

// Hard clean — X3 HALF+resync, X4 SSD1677 0xD7. Force Refresh / home scrub.
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

// New plate open (in-home menu, Settings first paint):
//   X3: FAST + OEM AA-pre-BW mid pulls (no black flash)
//   X4: HALF once — SSD1677 has no mid bank; FAST (0xFC) only drives diffs
//       and cannot lift residual. Cursor moves must stay on displayMenuFrame.
inline void displaySoftOpen(const GfxRenderer& renderer, int softCount = 1) {
  clearHardScrub();
  if (!gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    noteHalf();
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  if (softCount < 1) return;
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

// List screens that paint the full frame on every cursor move (Library,
// Recents). Must stay FAST — routing these through displaySoftOpen made every
// Down a HALF flash. X3 still gets the mid-bank settle; X4 cannot (no mid bank)
// so this is one full FAST. Do not window: SSD1677 displayWindow uses the 0xFC
// PART LUT on a strip and only resyncs RED for that strip, which ghosts the rest.
inline void displayFastFull(const GfxRenderer& renderer) {
  if (gpio.deviceIsX3()) {
    displaySoftOpen(renderer, /*softCount=*/1);
    return;
  }
  clearHardScrub();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Menu cursor / band: always plain full-frame FAST. Never soft, never HALF.
inline void displayMenuBand(const GfxRenderer& renderer, int x, int y, int w, int h) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
  (void)winX;
  (void)winY;
  (void)winW;
  (void)winH;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
