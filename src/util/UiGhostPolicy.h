#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

// UI display policy for home / settings / menus (not reader page turns).
//
// Reader Anti-Ghosting (SETTINGS.refreshFrequency) is applied only by
// ReaderUtils::displayWithRefreshCycle.
//
// Balance
// -------
// Hard HALF (X3 resync flash) is for *transitions only*: open/close menu,
// enter/leave Settings, Force Refresh, full home shell. Cursor / list scrolling
// must stay FAST forever — a mid-scroll HALF every N steps feels like "random
// black flashes while navigating." Residual on pure UI is acceptable; users
// can Force Refresh.

namespace UiGhostPolicy {

namespace detail {
inline bool& hardScrubArmed() {
  static bool armed = false;
  return armed;
}
}  // namespace detail

inline void noteHalf() { detail::hardScrubArmed() = false; }

// Next full-frame paint must hard-scrub (Force Refresh, home return, menu open,
// Settings enter). Cleared after the scrub runs.
inline void requestHardScrub() { detail::hardScrubArmed() = true; }

inline bool hardScrubArmed() { return detail::hardScrubArmed(); }

// Hard clean — X3 HALF + HalDisplay resync. Visible flash; use sparingly.
inline void displayHalf(const GfxRenderer& renderer) {
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  noteHalf();
}

// Full-frame UI (settings list, popup shell): FAST unless a transition armed scrub.
// Never auto-HALF after N list steps — that caused mid-menu flash storms.
inline void displayMenuFrame(const GfxRenderer& renderer) {
  if (detail::hardScrubArmed()) {
    displayHalf(renderer);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Menu *cursor* / band only: always snappy FAST. Never HALF.
inline void displayMenuBand(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    // X3 has no reliable partial for UI bands — full FAST, still no hard scrub.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

// Home under-panel scroll: snappy FAST only (no mid-scroll HALF).
inline void displayHomeUnderUpdate(const GfxRenderer& renderer, int winX, int winY, int winW, int winH) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(winX, winY, winW, winH);
  }
}

// Full-frame resume/shell helper (same as menu frame).
inline void displaySoftFull(const GfxRenderer& renderer) { displayMenuFrame(renderer); }

// Partial clock / strip: snappy only.
inline void displayPartialOrSoft(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

}  // namespace UiGhostPolicy
