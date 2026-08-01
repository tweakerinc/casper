#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

// Lightweight display helpers for home / settings / menus.
//
// Reader Anti-Ghosting (SETTINGS.refreshFrequency, default 15) is ONLY applied
// by ReaderUtils::displayWithRefreshCycle while reading a book.
//
// Home under-panel (Penumbra Recents / stats strip)
// ------------------------------------------------
// Every under-panel scroll on X3 uses displayGrayscaleBase(FAST) — the OEM
// AA-pre-BW(mid) bank in differential mode. That is a *soft* reinforce: strong
// BW/WB on changed ink, gentle WW/BB on "white stays white" so residual black
// is pulled down without a full HALF black flash. Full menus/settings stay on
// plain FAST so we do not precondition the whole UI the way that muddied white
// when greyscale-base was spammed everywhere.
//
// X4: true windowed FAST for the dirty band (no soft bank); periodic full HALF
// is not used on under-scroll (avoids hard flashes).
//
// Hard scrub: force-refresh / first Penumbra HALF baseline / reader interval.

namespace UiGhostPolicy {

// Home under-panel / Recents focus scroll. Upper chrome must remain valid in FB
// (redraw only dirties the under band; full soft path re-latches the whole panel).
inline void displayHomeUnderUpdate(const GfxRenderer& renderer, int winX, int winY, int winW, int winH) {
  if (gpio.deviceIsX3()) {
    // Soft anti-ghost every under-panel scroll (pull residual black out of white).
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(winX, winY, winW, winH);
  }
}

// Full-frame UI paint: settings lists, popup menus, home shell.
// Plain FAST — no greyscale-base (keeps menus snappy, avoids mid-bank mud).
inline void displayMenuFrame(const GfxRenderer& renderer) {
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Windowed menu band (home menu cursor move).
inline void displayMenuBand(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

// Snappy full-frame FAST (resume, clock) — no soft bank, no HALF.
inline void displaySoftFull(const GfxRenderer& renderer) {
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// True partial when the panel supports it; X3 falls back to full FAST.
inline void displayPartialOrSoft(const GfxRenderer& renderer, int x, int y, int w, int h) {
  if (gpio.deviceIsX3()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(x, y, w, h);
  }
}

}  // namespace UiGhostPolicy
