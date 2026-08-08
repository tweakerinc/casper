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
// Home is the cleanup hub: full-shell Home paints use hard HALF so residual
// from Menu / Settings / Library / Reader is scrubbed. People live on Home
// and in the book; they do not need every sub-screen to flash.
//
// Menu, Settings, Library, Recents, popups: always FAST full frames (and FAST
// list scroll forever). Mid-scroll HALF felt like random black flashes.
// Residual on those screens is acceptable until the next Home scrub.

namespace UiGhostPolicy {

namespace detail {
inline bool& hardScrubArmed() {
  static bool armed = false;
  return armed;
}
}  // namespace detail

inline void noteHalf() { detail::hardScrubArmed() = false; }

// Arm hard HALF for the next full-frame paint (Home resume, Force Refresh).
inline void requestHardScrub() { detail::hardScrubArmed() = true; }

// Drop a pending scrub so a menu/library frame can FAST over residual.
inline void clearHardScrub() { detail::hardScrubArmed() = false; }

inline bool hardScrubArmed() { return detail::hardScrubArmed(); }

// Hard clean — X3 HALF + HalDisplay resync. Home full shells + Force Refresh.
inline void displayHalf(const GfxRenderer& renderer) {
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  noteHalf();
}

// Full-frame UI (settings / library / popup): FAST unless a scrub was armed
// (should be rare — menus clear scrub on enter). List nav stays FAST-only.
inline void displayMenuFrame(const GfxRenderer& renderer) {
  if (detail::hardScrubArmed()) {
    displayHalf(renderer);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Always snappy FAST full frame (menu / Settings / Library open). Clears any
// leftover scrub arm so a prior Home scrub does not flash these screens.
inline void displayFastFull(const GfxRenderer& renderer) {
  clearHardScrub();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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

// Soft full frame (honors scrub arm if set).
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
