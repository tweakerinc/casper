#include "IrTokenizer.h"

#include <Utf8.h>

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

// True if a byte range could contain a codepoint utf8IsCjkBreakable() accepts.
// Keeps the per-codepoint scan below off the hot path for non-CJK books.
//
// The breakable set starts at U+1100 (Hangul Jamo) and is otherwise >= U+3000, so
// in UTF-8 the lead bytes that matter are 0xE1 and 0xE3..0xEF, plus 0xF0+ for the
// Extension B/C planes. Continuation bytes are 0x80-0xBF and never collide.
//
// 0xE2 is deliberately excluded: it covers U+2000-U+2FFF (General Punctuation,
// arrows, maths), which contains NO breakable CJK. That range is exactly where
// ordinary Latin typography lives — em dashes, curly quotes, ellipses — so a naive
// ">= 0xE0" test would have dragged most real prose onto the slow path for nothing.
bool mayContainCjk(const char* p, const char* end) {
  for (const char* q = p; q < end; ++q) {
    const unsigned char b = static_cast<unsigned char>(*q);
    if (b == 0xE1 || b >= 0xE3) return true;
  }
  return false;
}

}  // namespace

void tokenizeRuns(const ChapterIr& ch, const uint16_t runBegin, const uint16_t runCount, const uint16_t startRun,
                  const uint16_t startByte, std::vector<IrTok>& out) {
  out.clear();
  const auto& runs = ch.runs();
  const uint16_t runEnd = static_cast<uint16_t>(runBegin + runCount);

  // Reserve up front: this runs per block on EVERY page layout, and an unreserved
  // push_back loop costs a chain of geometric reallocations (allocate + copy +
  // free each time) that fragments DRAM — the one thing the C3 cannot afford.
  // Estimate from the remaining byte span at ~1 token per 4 bytes (a word plus its
  // space averages well above that in Latin prose), clamped so a pathological run
  // list cannot reserve something silly. Over-estimating slightly is free; the
  // vector is a per-layout scratch that is cleared and refilled.
  {
    size_t remainingBytes = 0;
    for (uint16_t ri = startRun < runBegin ? runBegin : startRun; ri < runEnd && ri < runs.size(); ++ri) {
      remainingBytes += runs[ri].textLen;
    }
    size_t estimate = remainingBytes / 4 + 8;
    if (estimate > 4096) estimate = 4096;
    if (out.capacity() < estimate) out.reserve(estimate);
  }
  uint16_t ri = startRun < runBegin ? runBegin : startRun;
  uint16_t bo = (ri == startRun) ? startByte : 0;
  for (; ri < runEnd && ri < runs.size(); ++ri, bo = 0) {
    const Run& run = runs[ri];
    if (bo >= run.textLen) continue;
    const char* base = ch.runText(run);
    // Do not insert a synthetic space between adjacent word-char runs.
    //
    // v22 IR already keeps a real leading space after </i>/</b>/</span> when
    // HTML had one ("Vampire"+" skill"). A style-run boundary with no space
    // is adjacency: ordinal suffixes (6 + superscript th) and mid-word bold.
    // The old backstop turned that adjacency into justify glue, so "th" flew
    // to the right margin on a fully-justified line.
    const char* p = base + bo;
    const char* end = base + run.textLen;
    while (p < end) {
      if (*p == ' ' || *p == '\t') {
        out.push_back(IrTok{ri, static_cast<uint16_t>(p - base), 1, true});
        ++p;
        continue;
      }
      const char* w0 = p;
      while (p < end && *p != ' ' && *p != '\t') ++p;
      // Latin fast path: no CJK possible, so the whole space-delimited run is one
      // token, exactly as before.
      if (!mayContainCjk(w0, p)) {
        out.push_back(IrTok{ri, static_cast<uint16_t>(w0 - base), static_cast<uint16_t>(p - w0), false});
        continue;
      }
      // CJK-bearing: re-walk this word by codepoint and cut at every legal break
      // opportunity, so the line fitter has somewhere to wrap.
      {
        const char* const wordEnd = p;
        const char* segStart = w0;
        const char* cur = w0;
        uint32_t prevCp = 0;
        while (cur < wordEnd) {
          const auto* q = reinterpret_cast<const unsigned char*>(cur);
          const uint32_t cp = utf8NextCodepoint(&q);
          const char* next = reinterpret_cast<const char*>(q);
          if (cp == 0 || next <= cur) {
            ++cur;  // malformed byte: step over it rather than spin
            continue;
          }
          if (next > wordEnd) next = wordEnd;
          if (prevCp != 0 && cur > segStart && hasCjkBreakBetween(prevCp, cp)) {
            out.push_back(
                IrTok{ri, static_cast<uint16_t>(segStart - base), static_cast<uint16_t>(cur - segStart), false});
            segStart = cur;
          }
          prevCp = cp;
          cur = next;
        }
        if (wordEnd > segStart) {
          out.push_back(
              IrTok{ri, static_cast<uint16_t>(segStart - base), static_cast<uint16_t>(wordEnd - segStart), false});
        }
      }
    }
  }
}

}  // namespace rivulet
