#pragma once

#include <cstdint>

#include "IrFormat.h"

namespace rivulet {

// Source Serif 4 + Literata ladders only (v1 validation fonts).
// IDs must match src/fontIds.h / StyleResolve tables.

struct FontLadder {
  static constexpr int kLen = 6;  // 8,10,12,14,16,18

  // Returns fontId for baseFontId's family at SizeStep, or baseFontId if unknown family.
  static int resolve(int baseFontId, SizeStep step);

  // Face style bits for GfxRenderer / EpdFontFamily.
  static uint8_t epdStyleBits(RunStyle style);
};

}  // namespace rivulet
