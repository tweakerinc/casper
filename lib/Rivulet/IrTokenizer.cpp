#include "IrTokenizer.h"

#include <Utf8.h>
#include "../Memory/BoundedUtf8.h"

#include <algorithm>
#include <cstdint>

namespace rivulet {
namespace {

// CJK line breaking. Ported from the classic ParsedText, which Rivulet replaces.
//
// CJK prose has no spaces, so a space-only tokenizer turns an entire paragraph
// into ONE token. It can never fit a line, hyphenation cannot split it, and the
// layouter's "emit the whole word anyway" fallback then paints the whole
// paragraph as a single overflowing line. CJK books were effectively unreadable.
//
// Breaks are allowed between CJK characters except where punctuation forbids it:
// closing punctuation may not start a line, opening punctuation may not end one.
bool isNoBreakBeforeCjkPunct(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isNoBreakAfterCjkPunct(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
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

bool hasCjkBreakBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunct(leftCp) || isNoBreakBeforeCjkPunct(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

}  // namespace

IrTokenCursor::IrTokenCursor(const ChapterIr& ch, uint16_t begin, uint16_t count,
                             uint16_t startRun, uint16_t startByte)
    : chapter_(&ch), run_(std::max(begin, startRun)), endRun_(static_cast<uint32_t>(begin) + count),
      byte_(run_ == startRun ? startByte : 0) {
  failed_ = endRun_ > ch.runs().size() || run_ > endRun_;
}

bool IrTokenCursor::next(IrTok& out) {
  if (failed_) return false;
  const auto& runs = chapter_->runs();
  while (run_ < endRun_) {
    const Run run = runs[run_];
    if (byte_ >= run.textLen) { ++run_; byte_ = 0; continue; }
    if (run.textOff > chapter_->textSize() || run.textLen > chapter_->textSize() - run.textOff) {
      failed_ = true; return false;
    }
    const char* base = chapter_->runText(run);
    const char* p = base + byte_;
    const char* start = p;
    const char* end = base + run.textLen;
    const bool space = *p == ' ' || *p == '\t';
    if (space) ++p;
    else {
      uint32_t prev = 0;
      while (p < end && *p != ' ' && *p != '\t') {
        const char* q = p;
        const uint32_t cp = casper_memory::nextUtf8(q, end);
        if (prev && hasCjkBreakBetween(prev, cp)) break;
        prev = cp; p = q;
      }
    }
    out = IrTok{static_cast<uint16_t>(run_), byte_, static_cast<uint16_t>(p - start), space};
    byte_ = static_cast<uint16_t>(p - base);
    return true;
  }
  return false;
}

void tokenizeRuns(const ChapterIr& ch, uint16_t begin, uint16_t count, uint16_t run,
                  uint16_t byte, std::vector<IrTok>& out) {
  // Compatibility helper for host tests; the device layouter uses the bounded
  // cursor directly. No whole-block reserve is made in the device path.
  out.clear();
  IrTokenCursor cursor(ch, begin, count, run, byte);
  IrTok token;
  while (cursor.next(token)) out.push_back(token);
}

}  // namespace rivulet
