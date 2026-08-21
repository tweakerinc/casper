#pragma once

#include <cstdint>
#include <vector>

#include "ChapterIr.h"

namespace rivulet {

// One tokenizer output. `w` is a layout memo (-1 = unmeasured); tokenizeRuns
// leaves it at -1. PageLayouter reuses this vector so it does not copy every
// token into a second type on the page-turn path.
//
// Only word tokens cache width. Space widths depend on flanking codepoints
// (and hyphenation can rewrite a neighbour), so they stay uncached.
// Reset w to -1 whenever byteOff/byteLen are rewritten.
struct IrTok {
  uint16_t runIndex = 0;
  uint16_t byteOff = 0;
  uint16_t byteLen = 0;
  bool space = false;
  int16_t w = -1;
};

// Split a block's runs into words and real spaces. Adjacent style runs with no
// space in the IR stay adjacent — "6" + superscript "th" is one visual word.
void tokenizeRuns(const ChapterIr& ch, uint16_t runBegin, uint16_t runCount, uint16_t startRun, uint16_t startByte,
                  std::vector<IrTok>& out);

}  // namespace rivulet
