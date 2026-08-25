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

// Wake moon→dots uses the same windowed/FAST path. If sleep left greys on
// glass, that push would flatten them the same way.
inline bool shouldPushWakeDotsWindow(const bool sleepLeftGreyscaleOnGlass) { return !sleepLeftGreyscaleOnGlass; }

}  // namespace qrsleep
