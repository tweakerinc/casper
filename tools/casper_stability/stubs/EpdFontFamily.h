#pragma once
#include "EpdFont.h"
class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR=0,BOLD=1,ITALIC=2,BOLD_ITALIC=3,UNDERLINE=4,STRIKETHROUGH=8,SUP=16,SUB=32,DROP_CAP=64 };
  static constexpr uint8_t TEXT_DECORATION_MASK = 12;
  const EpdGlyph* getGlyph(uint32_t, Style = REGULAR) const { static EpdGlyph g; return &g; }
  const EpdFontData* getData(Style=REGULAR) const { return &data; }
  EpdFontData data{};
};
