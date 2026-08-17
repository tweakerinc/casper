#include "FontLadder.h"

#include <EpdFontFamily.h>

#include <algorithm>
#include <cmath>

namespace rivulet {
namespace {

// Must stay in lockstep with lib/Epub/css/StyleResolve.cpp and src/fontIds.h.
constexpr int kLiterata[6] = {
    -1128177077,  // 8 → 10
    -1128177077,  // 10
    2090520927,   // 12
    -847079762,   // 14
    -209681255,   // 16
    -209681255,   // 18 → 16
};
constexpr int kSourceSerif[6] = {
    1470095001,   // 8
    -324599973,   // 10
    876380291,    // 12
    426921930,    // 14
    1484141743,   // 16
    652444703,    // 18
};

int familyIndex(const int* ladder, const int fontId) {
  for (int i = 0; i < 6; ++i) {
    if (ladder[i] == fontId) return i;
  }
  return -1;
}

}  // namespace

int FontLadder::resolve(const int baseFontId, const SizeStep step) {
  const int* ladder = nullptr;
  int baseIdx = familyIndex(kLiterata, baseFontId);
  if (baseIdx >= 0) {
    ladder = kLiterata;
  } else {
    baseIdx = familyIndex(kSourceSerif, baseFontId);
    if (baseIdx >= 0) ladder = kSourceSerif;
  }
  if (!ladder) return baseFontId;
  const int delta = static_cast<int>(step) - static_cast<int>(SizeStep::Body);
  const int idx = std::clamp(baseIdx + delta, 0, 5);
  return ladder[idx];
}

uint8_t FontLadder::epdStyleBits(const RunStyle style) {
  switch (style) {
    case RunStyle::Bold:
      return static_cast<uint8_t>(EpdFontFamily::BOLD);
    case RunStyle::Italic:
      return static_cast<uint8_t>(EpdFontFamily::ITALIC);
    case RunStyle::BoldItalic:
      return static_cast<uint8_t>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC);
    case RunStyle::Regular:
    default:
      return static_cast<uint8_t>(EpdFontFamily::REGULAR);
  }
}

}  // namespace rivulet
