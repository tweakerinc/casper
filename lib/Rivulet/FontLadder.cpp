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

// RunStyle mirrors EpdFontFamily::Style bit-for-bit so this stays a cast and the
// two definitions cannot drift apart as decorations are added.
static_assert(static_cast<uint8_t>(RunStyle::Bold) == static_cast<uint8_t>(EpdFontFamily::BOLD));
static_assert(static_cast<uint8_t>(RunStyle::Italic) == static_cast<uint8_t>(EpdFontFamily::ITALIC));
static_assert(static_cast<uint8_t>(RunStyle::BoldItalic) == static_cast<uint8_t>(EpdFontFamily::BOLD_ITALIC));
static_assert(static_cast<uint8_t>(RunStyle::Underline) == static_cast<uint8_t>(EpdFontFamily::UNDERLINE));
static_assert(static_cast<uint8_t>(RunStyle::Strikethrough) == static_cast<uint8_t>(EpdFontFamily::STRIKETHROUGH));
static_assert(static_cast<uint8_t>(RunStyle::Superscript) == static_cast<uint8_t>(EpdFontFamily::SUP));
static_assert(static_cast<uint8_t>(RunStyle::Subscript) == static_cast<uint8_t>(EpdFontFamily::SUB));

uint8_t FontLadder::epdStyleBits(const RunStyle style) { return static_cast<uint8_t>(style); }

}  // namespace rivulet
