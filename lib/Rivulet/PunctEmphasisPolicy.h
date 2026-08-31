#pragma once

#include <cstdint>

// First-letter polyfills in tradepubs wrap only the first *character* of a
// heading. Dungeon Crawler Carl chapter markers are "[ 1 ]" / "Chapter [1]",
// so that character is "[". The reader then paints a bold (or 2em) "[" and
// leaves "1]" in the surrounding face — one bracket looks randomly heavy.
//
// Skip the extra emphasis when the span/b/strong inner text is only opening
// punctuation (and whitespace). Real drop-caps ("C" + "hapter") and real bold
// groups ("[1]") still apply.
namespace punctemph {

inline constexpr bool isOpeningPunctuation(const uint32_t cp) {
  switch (cp) {
    case '[':
    case '(':
    case '{':
    case '<':
    case '"':
    case '\'':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

inline constexpr bool isAsciiWs(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Inner text of an emphasis tag, already stripped of markup. True only when
// every non-space codepoint is opening punctuation and at least one exists.
inline bool textIsOnlyOpeningPunctuation(const char* s, const size_t n) {
  if (!s || n == 0) return false;
  bool sawPunct = false;
  size_t i = 0;
  while (i < n) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      if (isAsciiWs(s[i])) {
        ++i;
        continue;
      }
      if (!isOpeningPunctuation(b)) return false;
      sawPunct = true;
      ++i;
      continue;
    }
    // Minimal UTF-8: we only accept the opening-punct set above.
    uint32_t cp = 0;
    size_t adv = 0;
    if ((b & 0xE0) == 0xC0 && i + 1 < n) {
      const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
      cp = (static_cast<uint32_t>(b & 0x1F) << 6) | (b1 & 0x3F);
      adv = 2;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < n) {
      const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
      cp = (static_cast<uint32_t>(b & 0x0F) << 12) | (static_cast<uint32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
      adv = 3;
    } else {
      return false;
    }
    if (!isOpeningPunctuation(cp)) return false;
    sawPunct = true;
    i += adv;
  }
  return sawPunct;
}

}  // namespace punctemph
