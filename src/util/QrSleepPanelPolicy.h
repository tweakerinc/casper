#pragma once

#include <cstdint>

// Quick Resume last-frame sleep keeps whatever is on glass and inks a moon.
// After a 2-bit greyscale pass (home covers, reader AA, clock AA) the live
// framebuffer is restored BW while glass still holds the grey planes.
// FAST diffs those planes into a dark/black plate; HALF replaces them with
// the BW framebuffer. Either push loses the grey pass. Skip the panel update
// and leave the last grey plate (moon stays in the framebuffer only).
namespace qrsleep {

enum class PanelPush : uint8_t { Skip, Fast };

inline PanelPush panelPush(const bool panelHoldsGreyscale) {
  return panelHoldsGreyscale ? PanelPush::Skip : PanelPush::Fast;
}

inline bool shouldPushMoonWindow(const bool panelHoldsGreyscale) {
  return panelPush(panelHoldsGreyscale) == PanelPush::Fast;
}

// Wake used to FAST moon→dots before first ink. X4 begin() AUTO_WRITE-fills
// both RAM planes white; a windowed dots FAST then whites the plate except
// the icon strip ("white screen then the page"). X3's path is a full-frame
// FAST of the sleep buffer — an extra refresh before the page. First ink
// already FAST-diffs the seeded sleep frame, so skip the dots push on both.
inline bool shouldPushWakeDotsWindow(const bool /*sleepLeftGreyscaleOnGlass*/) { return false; }

// X4 first FAST after wake is promoted to HALF while _isScreenOn is false
// (deep sleep discarded RAM). HALF is an absolute clean — white flash, ~1.7s.
// A full-frame displayWindow of the restored sleep buffer uses the FAST
// window path (no HALF promote), diffs against cleanup-seeded RED (zero
// change on glass), and arms _isScreenOn so the page FAST is differential.
// X3 skipInitialResync already allows that first FAST; do not prime there.
inline bool shouldPrimeWakeBaseline(const bool x4Panel) { return x4Panel; }

}  // namespace qrsleep
