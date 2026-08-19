#include "PageLayouter.h"

#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Epub/hyphenation/Hyphenator.h"
#include "FontLadder.h"

namespace rivulet {
namespace {

struct Tok {
  uint16_t runIndex = 0;
  uint16_t byteOff = 0;
  uint16_t byteLen = 0;
  bool space = false;
  // Memoized advance width for this token, -1 = not measured yet.
  //
  // Every word used to be measured twice per layout: once by the line-fitting
  // loop and again by the span-emit loop, with identical (fontId, style, bytes)
  // inputs — and a word pushed to the next line was measured a third time.
  // measureWord() walks every codepoint through getTextAdvanceX (map lookup,
  // glyph lookup, kerning), and layoutPage runs on EVERY page turn (Rivulet
  // lays out on demand rather than reading precomputed geometry), so this was
  // pure duplicated work on the hot path.
  //
  // Only word tokens are cached. Space widths depend on their flanking
  // codepoints, which change when hyphenation rewrites a neighbour, so they
  // stay uncached (measureInterWordSpace is far cheaper anyway).
  // MUST be reset to -1 whenever byteOff/byteLen are rewritten.
  int16_t w = -1;
};

int lineH(const GfxRenderer& r, const int baseFontId, const SizeStep step, const float lc) {
  const int fid = FontLadder::resolve(baseFontId, step);
  int h = r.getLineHeight(fid, lc);
  if (h < 8) {
    // Font missing / zero metrics — fall back so lines never stack on top of each other.
    h = std::max(12, r.getFontAscenderSize(baseFontId) + 4);
    h = static_cast<int>(h * (lc > 0.1f ? lc : 1.0f) + 0.5f);
  }
  return std::max(10, h);
}

int measureWord(const GfxRenderer& r, const int fontId, const EpdFontFamily::Style st, const char* s, const size_t n) {
  if (!s || n == 0) return 0;
  char buf[128];
  if (n < sizeof(buf)) {
    std::memcpy(buf, s, n);
    buf[n] = '\0';
    return r.getTextAdvanceX(fontId, buf, st, 0);
  }
  int w = 0;
  for (size_t i = 0; i < n;) {
    const size_t c = std::min(n - i, sizeof(buf) - 1);
    std::memcpy(buf, s + i, c);
    buf[c] = '\0';
    w += r.getTextAdvanceX(fontId, buf, st, 0);
    i += c;
  }
  return w;
}

// First / last scalar of a word token (for inter-word space kerning).
uint32_t tokenFirstCp(const ChapterIr& ch, const Tok& t) {
  if (t.space || t.byteLen == 0 || t.runIndex >= ch.runs().size()) return 0;
  const char* p = ch.runText(ch.runs()[t.runIndex]) + t.byteOff;
  const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
  return utf8NextCodepoint(&u);
}

uint32_t tokenLastCp(const ChapterIr& ch, const Tok& t) {
  if (t.space || t.byteLen == 0 || t.runIndex >= ch.runs().size()) return 0;
  const char* base = ch.runText(ch.runs()[t.runIndex]) + t.byteOff;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(base);
  const unsigned char* end = p + t.byteLen;
  uint32_t last = 0;
  while (p < end) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || p == before) break;
    last = cp;
  }
  return last;
}

// Inter-word gap: classic ParsedText uses getSpaceAdvance (space + flanking kern).
// getSpaceAdvance already floors negative kern; we never go below that floor here.
int measureInterWordSpace(const GfxRenderer& r, const int fontId, const EpdFontFamily::Style st,
                          const uint32_t leftCp, const uint32_t rightCp) {
  const int base = r.getSpaceWidth(fontId, st);
  const int basePx = base > 0 ? base : 1;
  if (leftCp == 0 || rightCp == 0) return basePx;
  return r.getSpaceAdvance(fontId, leftCp, rightCp, st);
}

bool isAsciiWordByte(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Last byte of previous non-empty run content that is a "word" char (Latin).
// Used to heal style-boundary glue when IR dropped the inter-run space.
bool runEndsWithWordChar(const ChapterIr& ch, const Run& run) {
  if (run.textLen == 0) return false;
  const char* base = ch.runText(run);
  // Walk last UTF-8 scalar; treat any non-ASCII letter-like as word (Cyrillic etc.).
  const unsigned char* p = reinterpret_cast<const unsigned char*>(base);
  const unsigned char* end = p + run.textLen;
  uint32_t last = 0;
  while (p < end) {
    const unsigned char* before = p;
    last = utf8NextCodepoint(&p);
    if (last == 0 || p == before) break;
  }
  if (last == 0) return false;
  if (last < 128) return isAsciiWordByte(static_cast<unsigned char>(last));
  // Non-ASCII: treat as word if not common punctuation/space.
  return last != 0x00A0 && last != 0x2013 && last != 0x2014 && last != 0x2026;
}

bool runStartsWithWordChar(const ChapterIr& ch, const Run& run, const uint16_t byteOff) {
  if (byteOff >= run.textLen) return false;
  const char* base = ch.runText(run) + byteOff;
  const unsigned char* u = reinterpret_cast<const unsigned char*>(base);
  const uint32_t cp = utf8NextCodepoint(&u);
  if (cp == 0) return false;
  if (cp < 128) return isAsciiWordByte(static_cast<unsigned char>(cp));
  return cp != 0x00A0;
}

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
    case '.': case ',': case ':': case ';': case '!': case '?':
    case ')': case ']': case '}':
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
    case '(': case '[': case '{':
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

void tokenizeFrom(const ChapterIr& ch, const uint16_t runBegin, const uint16_t runCount, const uint16_t startRun,
                  const uint16_t startByte, std::vector<Tok>& out) {
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
  int prevRunWithText = -1;
  for (; ri < runEnd && ri < runs.size(); ++ri, bo = 0) {
    const Run& run = runs[ri];
    if (bo >= run.textLen) continue;
    const char* base = ch.runText(run);
    // Heal missing space between style runs: "Vampire" + "skill" (old IR stripped
    // the leading space after </i>). Do not inject before punctuation (": You've").
    if (prevRunWithText >= 0 && bo == 0 && runStartsWithWordChar(ch, run, 0) &&
        runEndsWithWordChar(ch, runs[static_cast<size_t>(prevRunWithText)])) {
      // Synthetic space: reuse current runIndex with byteLen 0 + space flag is wrong
      // for measure; use a 1-byte space from this run only if present — else mark
      // space with byteLen 0 and special-case measure… Prefer: space token that
      // measureInterWordSpace handles (t.space true, text ignored).
      out.push_back(Tok{ri, 0, 0, true});
    }
    const char* p = base + bo;
    const char* end = base + run.textLen;
    bool emitted = false;
    while (p < end) {
      if (*p == ' ' || *p == '\t') {
        out.push_back(Tok{ri, static_cast<uint16_t>(p - base), 1, true});
        ++p;
        emitted = true;
        continue;
      }
      const char* w0 = p;
      while (p < end && *p != ' ' && *p != '\t') ++p;
      // Latin fast path: no CJK possible, so the whole space-delimited run is one
      // token, exactly as before.
      if (!mayContainCjk(w0, p)) {
        out.push_back(Tok{ri, static_cast<uint16_t>(w0 - base), static_cast<uint16_t>(p - w0), false});
        emitted = true;
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
                Tok{ri, static_cast<uint16_t>(segStart - base), static_cast<uint16_t>(cur - segStart), false});
            segStart = cur;
          }
          prevCp = cp;
          cur = next;
        }
        if (wordEnd > segStart) {
          out.push_back(
              Tok{ri, static_cast<uint16_t>(segStart - base), static_cast<uint16_t>(wordEnd - segStart), false});
        }
      }
      emitted = true;
    }
    if (emitted) prevRunWithText = static_cast<int>(ri);
  }
}

// Closing punctuation that must not start a line alone ("distinction." → not "distinction" / ".").
bool isClosingOrphanPunctCp(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case '%':
    case ')':
    case ']':
    case '}':
    case '\'':
    case '"':
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x00BB:  // »
    case 0x203A:  // ›
    case 0x2026:  // …
    case 0x00B0:  // °
      return true;
    default:
      return false;
  }
}

bool bytesAreOnlyClosingPunct(const char* s, const size_t n) {
  if (!s || n == 0) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* end = p + n;
  bool any = false;
  while (p < end) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || p == before) return false;
    if (cp == 0x200B || cp == 0xFEFF) continue;  // zero-width
    if (!isClosingOrphanPunctCp(cp)) return false;
    any = true;
  }
  return any;
}

bool tokenIsOnlyClosingPunct(const ChapterIr& ch, const Tok& t) {
  if (t.space || t.byteLen == 0 || t.runIndex >= ch.runs().size()) return false;
  const Run& run = ch.runs()[t.runIndex];
  if (t.byteOff >= run.textLen) return false;
  const size_t n = std::min<size_t>(t.byteLen, run.textLen - t.byteOff);
  return bytesAreOnlyClosingPunct(ch.runText(run) + t.byteOff, n);
}

// Optional hyphen-style break only — never force-split ordinary words mid-letter.
// If this returns false, the whole word drops to the next line (normal wrap).
// - Hard hyphens already in the word (Here’s-Some-Good-…) always allowed.
// - Dictionary / soft hyphens only when the Hyphenation setting is on.
// Never leaves only "." / "," / "!" on the next line.
bool tryHyphenateWordToFit(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style st,
                           const char* word, const size_t wordLen, const int remain, const bool hyphenationEnabled,
                           size_t& outPrefixBytes, std::string& outPrefixStorage, int& outPrefixW) {
  outPrefixBytes = 0;
  outPrefixStorage.clear();
  outPrefixW = -1;
  if (!word || wordLen < 2 || remain < 4) return false;

  const std::string hyphenWord(word, wordLen);
  const auto breaks = Hyphenator::breakOffsets(hyphenWord, /*includeFallback=*/hyphenationEnabled);
  size_t bestOff = 0;
  int bestW = -1;
  bool bestHyphen = true;
  for (const auto& br : breaks) {
    if (!hyphenationEnabled && br.requiresInsertedHyphen) {
      // Soft/pattern breaks only with hyphenation on; hard '-' still applies.
      continue;
    }
    if (br.byteOffset == 0 || br.byteOffset >= hyphenWord.size()) continue;
    if (bytesAreOnlyClosingPunct(word + br.byteOffset, wordLen - br.byteOffset)) continue;
    std::string prefix = hyphenWord.substr(0, br.byteOffset);
    if (br.requiresInsertedHyphen) prefix.push_back('-');
    const int pw = measureWord(renderer, fontId, st, prefix.data(), prefix.size());
    if (pw <= remain && pw > bestW) {
      bestW = pw;
      bestOff = br.byteOffset;
      bestHyphen = br.requiresInsertedHyphen;
    }
  }
  if (bestW <= 0) return false;
  outPrefixStorage = hyphenWord.substr(0, bestOff);
  if (bestHyphen) outPrefixStorage.push_back('-');
  outPrefixBytes = bestOff;
  outPrefixW = bestW;
  return true;
}

// First letter for drop-cap (Latin + Greek + Cyrillic, matching classic ParsedText peel).
// Advances runIndex/byteOff past the letter.
std::string takeDropLetter(const ChapterIr& ch, const Block& b, uint16_t& runIndex, uint16_t& byteOff) {
  const auto& runs = ch.runs();
  runIndex = b.runBegin;
  byteOff = 0;
  for (uint16_t ri = b.runBegin; ri < b.runBegin + b.runCount && ri < runs.size(); ++ri) {
    const Run& run = runs[ri];
    const unsigned char* p = reinterpret_cast<const unsigned char*>(ch.runText(run));
    const unsigned char* end = p + run.textLen;
    const unsigned char* start = p;
    while (p < end) {
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;
      if (cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x200B) continue;
      // Soft hyphen / combining — not a drop-cap host.
      if (cp == 0x00AD || (cp >= 0x0300 && cp <= 0x036F)) continue;
      const bool letter =
          (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F) ||
          (cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x0400 && cp <= 0x04FF);
      if (!letter) return {};
      std::string s;
      utf8AppendCodepoint(cp, s);
      runIndex = ri;
      byteOff = static_cast<uint16_t>(p - start);
      if (byteOff >= run.textLen) {
        runIndex = static_cast<uint16_t>(ri + 1);
        byteOff = 0;
      }
      return s;
    }
  }
  return {};
}

// Classic drop-cap: glyph height×scale fills a 2-line zone; paint y is line-box TOP
// of the first body line (+ small cap-height nudge). See ChapterHtmlSlimParser +
// GfxRenderer::drawText(DROP_CAP) (top-align to y, not baseline).
bool glyphInkMetrics(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style faceStyle,
                     const char* letter, int& outH, int& outTop) {
  outH = 0;
  outTop = 0;
  if (!letter || !letter[0]) return false;
  const auto& map = renderer.getFontMap();
  const auto it = map.find(fontId);
  if (it == map.end()) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(letter);
  const uint32_t cp = utf8NextCodepoint(&p);
  if (cp == 0) return false;
  const EpdGlyph* g = it->second.getGlyph(cp, faceStyle);
  if (!g || g->height == 0) return false;
  outH = static_cast<int>(g->height);
  outTop = static_cast<int>(g->top);
  return true;
}

// Publisher scene-break fallback: EPUBs pair a decorative separator (usually an
// inline <svg>, sometimes an unsupported raster) with a plain-text alternative
// like "* * *" or "· · ·" for readers that cannot draw it. We deliberately skip
// SVG (HtmlToIr) and 0x0 images (below), so that fallback is what the reader
// actually sees — it is book content, not a rendering failure. Treat it as a real
// thematic break (centred, with air) instead of a stray left-aligned body line.
bool isSceneBreakRun(const ChapterIr& ch, const Block& b) {
  if (b.kind != BlockKind::Paragraph || b.runCount == 0) return false;
  int glyphs = 0;
  const uint16_t runEnd = static_cast<uint16_t>(b.runBegin + b.runCount);
  for (uint16_t ri = b.runBegin; ri < runEnd && ri < ch.runs().size(); ++ri) {
    const Run& r = ch.runs()[ri];
    const char* text = ch.runText(r);
    if (!text) return false;
    const char* p = text;
    const char* end = text + r.textLen;
    while (p < end) {
      const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p));
      if (cp == 0) break;
      if (cp == ' ' || cp == '\t' || cp == 0x00A0) continue;
      switch (cp) {
        case '*':
        case '.':
        case '-':
        case '_':
        case '~':
        case 0x00B7:  // ·
        case 0x2013:  // –
        case 0x2014:  // —
        case 0x2022:  // •
        case 0x2042:  // ⁂
        case 0x2043:  // ⁃
        case 0x2731:  // ✱
        case 0x273B:  // ❋
          if (++glyphs > 12) return false;  // a real sentence, not a separator
          continue;
        default:
          return false;
      }
    }
  }
  return glyphs > 0;
}

bool atEnd(const ChapterIr& ch, const IrCursor& c) { return c.blockIndex >= ch.blocks().size(); }

void advancePastBlock(const ChapterIr& ch, IrCursor& c) {
  ++c.blockIndex;
  if (c.blockIndex < ch.blocks().size()) {
    c.runIndex = ch.blocks()[c.blockIndex].runBegin;
  } else {
    c.runIndex = 0;
  }
  c.byteInRun = 0;
}

}  // namespace

bool PageLayouter::layoutPage(const ChapterIr& chapter, const GfxRenderer& renderer, const LayoutParams& params,
                              const IrCursor& from, LaidOutPage& out) {
  out.clear();
  out.start = from;
  out.end = from;
  if (chapter.empty() || atEnd(chapter, from)) {
    out.atChapterEnd = true;
    return false;
  }

  const int baseFontId = params.key.fontId;
  const int viewW = params.key.viewportW;
  const int viewH = params.key.viewportH;
  if (viewW < 16 || viewH < 16) return false;

  const float lc = params.lineCompression > 0.1f ? params.lineCompression : 1.0f;
  const int bodyLine = lineH(renderer, baseFontId, SizeStep::Body, lc);
  const int bodyEm =
      std::max(8, params.bodyEmPx > 0 ? params.bodyEmPx : renderer.getFontAscenderSize(baseFontId));

  int y = 0;
  IrCursor cur = from;

  int dropW = 0;
  int dropBottom = 0;
  bool dropIsRight = false;  // true = figright (text on left, float on right)
  // CSS-style vertical margin collapse for book-style spacers: adjacent trailing
  // and leading margins collapse to max(a,b), not a+b (was stacking 1.4+1.4+2.8em).
  int trailingCollapsePx = 0;

  auto xBase = [&](const int lineTop) -> int {
    if (dropW <= 0) return 0;
    const int mid = lineTop + bodyLine / 2;
    if (mid >= dropBottom) return 0;
    // Right float: text still starts at x=0; left float: text starts after the plate.
    return dropIsRight ? 0 : dropW;
  };
  auto widthAt = [&](const int lineTop) -> int {
    if (dropW <= 0) return viewW;
    const int mid = lineTop + bodyLine / 2;
    if (mid >= dropBottom) return viewW;
    return std::max(16, viewW - dropW);
  };

  // bit0 = Book's Style (honor IR/CSS align + rhythm). Clear = force L/C/R/J for all text.
  // bits 2-3 = forced align when not book-style: 0=Justify 1=Left 2=Center 3=Right.
  // bit4 0x10 = hyphenation (layout-time word breaks).
  const bool bookStyle = (params.key.flags & 1) != 0;
  const bool hyphenationEnabled = (params.key.flags & 0x10) != 0;
  const bool forceUserAlign = !bookStyle;
  Align userAlign = Align::Justify;
  if (forceUserAlign) {
    switch ((params.key.flags >> 2) & 0x3) {
      case 0:
        userAlign = Align::Justify;
        break;
      case 1:
        userAlign = Align::Left;
        break;
      case 2:
        userAlign = Align::Center;
        break;
      case 3:
        userAlign = Align::Right;
        break;
      default:
        break;
    }
  }

  // Per-layout scratch, hoisted out of the block/line loops below so their
  // capacity is reused instead of being rebuilt from scratch. These used to be
  // declared inside the loops — `toks` per block and `lineToks` +
  // `hyphenPrefixStorage` per LINE — which meant a fresh allocate/grow/free cycle
  // for every line of every page turn.
  std::vector<Tok> toks;
  std::vector<Tok> lineToks;
  std::string hyphenPrefixStorage;  // owns the "prefix-" string for the current line

  // Reserve the span vector once. A page holds one GlyphSpan per word
  // (sizeof ≈ 48 B with its owned std::string), so an unreserved fill grew
  // through ~9 reallocations, and the last of those needs the old and new
  // buffers live at the same time — a transient contiguous demand of tens of KB
  // at exactly the moment the heap is tightest. Estimate lines × words-per-line,
  // assuming an average word occupies ~3em, and clamp so a tiny font or a huge
  // viewport cannot reserve something absurd (over-reserving GlyphSpan is
  // expensive, so the cap matters as much as the floor).
  // (skipped entirely on a measure-only pass — nothing is emitted, so reserving
  // would just hand back a ~14 KB allocation per page walked.)
  if (!params.measureOnly) {
    const int linesPerPage = std::max(1, viewH / std::max(1, bodyLine));
    const int wordsPerLine = std::max(1, viewW / std::max(1, bodyEm * 3));
    size_t spanEstimate = static_cast<size_t>(linesPerPage) * static_cast<size_t>(wordsPerLine);
    if (spanEstimate < 32) spanEstimate = 32;
    if (spanEstimate > 512) spanEstimate = 512;
    out.spans.reserve(spanEstimate);
  }

  // Process blocks until page full or chapter ends.
  while (!atEnd(chapter, cur) && y < viewH) {
    const Block& block = chapter.blocks()[cur.blockIndex];
    // Normalize cursor into block. Stale page-map cursors can carry a runIndex
    // from a previous IR rebuild that falls outside this block's run range —
    // without clamping, tokenizeFrom yields nothing and we skip all text,
    // marking chapter-end after a handful of empty pages.
    const uint16_t blockRunEnd = static_cast<uint16_t>(block.runBegin + block.runCount);
    if (cur.runIndex < block.runBegin || cur.runIndex >= blockRunEnd ||
        (block.runCount == 0 && cur.byteInRun != 0)) {
      cur.runIndex = block.runBegin;
      cur.byteInRun = 0;
    } else if (cur.runIndex < chapter.runs().size()) {
      const uint16_t runLen = chapter.runs()[cur.runIndex].textLen;
      if (cur.byteInRun > runLen) cur.byteInRun = 0;
    }
    const bool atBlockStart = (cur.runIndex == block.runBegin && cur.byteInRun == 0);
    const bool isHeading = (block.kind >= BlockKind::Heading1 && block.kind <= BlockKind::Heading6);
    // Book's Style: per-block IR align (CSS/heuristics from HtmlToIr).
    // Force L/C/R/J: same user align for every text block — no CSS "center this title".
    Align blockAlign = block.align;
    if (forceUserAlign &&
        (block.kind == BlockKind::Paragraph || isHeading)) {
      blockAlign = userAlign;
    }
    // Scene-break fallback text always centres, in every align mode — a forced
    // Left/Justify setting must not leave "* * *" hanging off the left margin.
    const bool sceneBreak = isSceneBreakRun(chapter, block);
    if (sceneBreak) blockAlign = Align::Center;

    if ((block.flags & kBlockForcePageBreak) != 0 && y > 0 && atBlockStart) {
      break;
    }

    // Drop-cap paragraph prefers top of page.
    if ((block.flags & kBlockDropCap) != 0 && y > 0 && atBlockStart) {
      break;
    }

    int marginTop = (block.marginTopEmQ4 * bodyEm) / 16;
    int marginBottom = (block.marginBottomEmQ4 * bodyEm) / 16;
    if (sceneBreak) {
      // Give it the air a drawn separator would have had, so it reads as a break
      // in the prose rather than a short orphan line.
      marginTop = std::max(marginTop, (y > 0) ? bodyLine : 0);
      marginBottom = std::max(marginBottom, bodyLine);
    }
    if (forceUserAlign) {
      // Plain modes: ignore CSS/book vertical rhythm; uniform simple gaps.
      if (block.kind == BlockKind::Spacer) {
        // Skip book-style spacers (implicit-break / alignment-block air).
        trailingCollapsePx = 0;
        advancePastBlock(chapter, cur);
        continue;
      }
      if (isHeading) {
        marginTop = (y > 0) ? bodyLine / 3 : 0;
        marginBottom = bodyLine / 4;
      } else if (block.kind == BlockKind::Paragraph) {
        marginTop = 0;
        if (params.extraParagraphSpacing) {
          marginBottom = extraParaGapPx(bodyLine, params.extraParagraphSpacingHeight);
        } else {
          marginBottom = std::max(2, bodyLine / 8);
        }
      } else if (block.kind == BlockKind::Image) {
        marginTop = bodyLine / 4;
        marginBottom = bodyLine / 4;
      } else {
        marginTop = 0;
        marginBottom = bodyLine / 6;
      }
    } else {
      // Book's Style: trust IR margins. Do not invent floors under headings that
      // live inside an hgroup (those are margin 0; air is the post-hgroup spacer).
      // Extra paragraph spacing ON: normalize every paragraph to the user's gap
      // (¼ / ½ / full line) — replace CSS para margins, do not stack on top of
      // them (max/add looked uneven: book gaps + user gaps on some paras only).
      if (params.extraParagraphSpacing && block.kind == BlockKind::Paragraph) {
        marginTop = 0;
        marginBottom = extraParaGapPx(bodyLine, params.extraParagraphSpacingHeight);
      }
    }
    // Apply top margin with collapse against previous block's trailing margin.
    // Collapse only in Book's Style (CSS adjacent-margin behavior).
    if (atBlockStart && (y > 0 || block.kind == BlockKind::Spacer)) {
      int applyTop = marginTop;
      if (bookStyle && trailingCollapsePx > 0) {
        // Collapse: only the excess of this top over the already-applied trailing.
        applyTop = std::max(0, marginTop - trailingCollapsePx);
      }
      y += applyTop;
      if (y >= viewH && block.kind != BlockKind::Spacer) break;
    }

    if (block.kind == BlockKind::HorizontalRule) {
      trailingCollapsePx = 0;
      if (y + bodyLine > viewH && y > 0) break;
      // Typeset rule: a short centred hairline with air either side, the way a
      // print book sets a section break. This used to be a literal row of em-dash
      // glyphs, which reads as typed-out punctuation and disappears entirely on a
      // font without U+2014.
      if (!params.measureOnly) {
        RulePlate rule;
        rule.w = static_cast<int16_t>(std::max(24, (viewW * 22) / 100));
        rule.x = static_cast<int16_t>((viewW - rule.w) / 2);
        rule.y = static_cast<int16_t>(y + bodyLine / 2);
        rule.h = static_cast<int16_t>(std::max(1, bodyEm / 12));
        out.rules.push_back(rule);
      }
      y += bodyLine + marginBottom;
      advancePastBlock(chapter, cur);
      continue;
    }

    if (block.kind == BlockKind::Image) {
      // Unsupported formats (SVG ornamental breaks) keep 0×0 after prepare — skip
      // entirely so we do not paint a white box above the "* * *" text fallback.
      if (block.imageW == 0 && block.imageH == 0) {
        advancePastBlock(chapter, cur);
        continue;
      }
      int iw = block.imageW > 0 ? static_cast<int>(block.imageW) : viewW;
      int ih = block.imageH > 0 ? static_cast<int>(block.imageH) : std::max(bodyLine * 4, viewH / 3);
      if (iw > viewW) {
        ih = std::max(1, (ih * viewW) / iw);
        iw = viewW;
      }
      // Classic placeFloatImage: letter-shrink ONLY for narrow LEFT floats
      // (Alice ornate C). figright / wide plates keep full size and wrap as figures.
      // Ornament (Fourth Wing .orn): small centered graphic, never a page-eating plate.
      const int maxLetterW = std::max(120, (viewW * 28) / 100);
      const bool leftFloat = (block.flags & kBlockFloatLeft) != 0;
      const bool rightFloat = (block.flags & kBlockFloatRight) != 0;
      const bool isOrnament = (block.flags & kBlockOrnament) != 0;
      if (isOrnament) {
        const int targetW = std::max(28, (viewW * 12) / 100);
        if (iw > targetW && iw > 0) {
          ih = std::max(1, (ih * targetW) / iw);
          iw = targetW;
        }
      }
      const bool letterGlyph =
          !isOrnament && leftFloat && iw > 0 && iw <= maxLetterW &&
          (ih <= 0 || ih <= bodyLine * 6 || iw <= ih * 2);
      const bool figureFloat = !isOrnament && (leftFloat || rightFloat) && !letterGlyph;

      if (letterGlyph) {
        // Seat letter in ~2 body lines (classic looksLikeDropCap).
        const int targetH = 2 * bodyLine;
        if (ih > targetH && ih > 0) {
          iw = std::max(1, (iw * targetH) / ih);
          ih = targetH;
        } else if (ih < bodyLine) {
          ih = targetH;
          if (iw < bodyLine) iw = std::min(maxLetterW, targetH);
        }
      } else if (!isOrnament) {
        // Full figures: fit viewport, cap height ~90% page.
        const int maxH = (viewH * 9) / 10;
        if (ih > maxH && ih > 0) {
          iw = std::max(1, (iw * maxH) / ih);
          ih = maxH;
        }
      }

      if (!letterGlyph && !figureFloat) {
        // Centered plate or ornament — need room for the box.
        if (y > 0 && !isOrnament && (ih > viewH / 2 || y + ih + marginBottom > viewH)) break;
        if (y + ih > viewH && y > 0) break;
      } else if (letterGlyph && y + 2 * bodyLine > viewH && y > 0) {
        break;
      } else if (figureFloat && y > 0 && y + std::min(ih, bodyLine * 3) > viewH) {
        // Need room for at least a few wrap lines beside a tall figure.
        break;
      }

      std::string href;
      if (block.runCount > 0 && block.runBegin < chapter.runs().size()) {
        href = chapter.runString(chapter.runs()[block.runBegin]);
      }
      // Illuminae / experimental EPUBs often emit the same <img> twice (wrapper +
      // inner). Drop consecutive identical hrefs at the same vertical band.
      if (!href.empty() && !out.images.empty()) {
        const ImagePlate& prev = out.images.back();
        if (prev.href == href && std::abs(static_cast<int>(prev.y) - y) <= bodyLine &&
            std::abs(static_cast<int>(prev.w) - iw) <= 4) {
          advancePastBlock(chapter, cur);
          continue;
        }
      }

      ImagePlate plate;
      plate.w = static_cast<int16_t>(iw);
      plate.h = static_cast<int16_t>(ih);
      plate.href = std::move(href);

      if (letterGlyph || figureFloat) {
        dropIsRight = rightFloat;
        plate.x = static_cast<int16_t>(rightFloat ? std::max(0, viewW - iw) : 0);
        plate.y = static_cast<int16_t>(y);
        out.images.push_back(std::move(plate));
        const int gap = letterGlyph ? std::max(2, bodyLine / 8) : std::max(4, bodyLine / 6);
        dropW = std::min(viewW * 55 / 100, iw + gap);
        const int zoneH = letterGlyph ? std::max(ih, 2 * bodyLine) : ih;
        dropBottom = y + zoneH;
        out.hasDropZone = true;
        out.dropZoneW = static_cast<int16_t>(dropW);
        out.dropZoneH = static_cast<int16_t>(zoneH);
        // Do NOT advance y — following text wraps beside the float.
        trailingCollapsePx = 0;
        advancePastBlock(chapter, cur);
        continue;
      }

      // Centered plate / ornament (CSS text-align center on h1 ornaments).
      plate.x = static_cast<int16_t>(std::max(0, (viewW - iw) / 2));
      plate.y = static_cast<int16_t>(y);
      out.images.push_back(std::move(plate));
      // Ornaments: tight air under the graphic so CHAPTER ONE sits just below.
      const int after = isOrnament ? std::max(2, bodyLine / 4) : marginBottom;
      y += ih + after;
      trailingCollapsePx = 0;
      advancePastBlock(chapter, cur);
      continue;
    }

    if (block.kind == BlockKind::Spacer) {
      // marginTop already applied (collapsed); add bottom and remember for next collapse.
      if (marginBottom > 0) {
        y += marginBottom;
        trailingCollapsePx = marginBottom;
      } else if (marginTop <= 0 && trailingCollapsePx <= 0) {
        y += bodyLine / 3;  // minimal floor if spacer carried no metrics
        trailingCollapsePx = 0;
      } else {
        // top-only spacer: trailing for collapse is the top we effectively used
        trailingCollapsePx = marginTop;
      }
      advancePastBlock(chapter, cur);
      continue;
    }
    if (block.runCount == 0) {
      y += std::max(bodyLine / 2, marginBottom);
      trailingCollapsePx = 0;
      advancePastBlock(chapter, cur);
      continue;
    }

    // Drop cap: TOP of letter aligns with body capital tops; ink fills ~2 lines.
    // Classic: metric face×scale + dropCapYAdjust (bodyLead). paint y = line-box TOP.
    uint16_t bodyRun = cur.runIndex;
    uint16_t bodyByte = cur.byteInRun;
    bool dropCapActive = false;
    Align dropBodyAlign = blockAlign;
    if ((block.flags & kBlockDropCap) != 0 && atBlockStart) {
      constexpr int kDropLines = 2;
      const int targetH = kDropLines * bodyLine;
      if (y + targetH > viewH && y > 0) break;

      uint16_t capRun = 0, capByte = 0;
      const std::string letter = takeDropLetter(chapter, block, capRun, capByte);
      if (!letter.empty()) {
        const int bodyFaceId = FontLadder::resolve(baseFontId, SizeStep::Body);
        const int maxW = std::max(20, (viewW * 45) / 100);
        // Stay under bottom of line 2 (user: "tiny tiny bit" under — no line-3 kiss).
        const int maxPaintH = std::max(bodyLine, targetH - std::max(4, bodyLine / 6));

        // Prefer body×2, then Plus1×2; scale 3 only if still under maxPaintH.
        int candidates[2] = {
            bodyFaceId,
            FontLadder::resolve(baseFontId, SizeStep::Plus1),
        };
        const int nCand = (candidates[1] != candidates[0]) ? 2 : 1;

        int bestFaceId = bodyFaceId;
        int bestScale = 2;
        int bestW = 0;
        int bestPaintH = 0;
        bool bestBold = true;
        bool found = false;

        auto tryFaceScale = [&](const int fontId, const bool useBold, const int scale) {
          if (scale < 2 || scale > 3) return;
          const auto faceStyle = useBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
          int gH = 0, gTop = 0;
          if (!glyphInkMetrics(renderer, fontId, faceStyle, letter.c_str(), gH, gTop)) return;
          const int paintH = gH * scale;
          if (paintH <= 0 || paintH > maxPaintH) return;
          const auto styleBits = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(faceStyle) |
                                                                  static_cast<uint8_t>(EpdFontFamily::DROP_CAP));
          const int w = renderer.getTextAdvanceX(fontId, letter.c_str(), styleBits, scale);
          if (w < 8 || w > maxW) return;
          // Tallest that still fits under maxPaintH; prefer scale 2 and body face.
          const bool better = !found || paintH > bestPaintH ||
                              (paintH == bestPaintH && scale < bestScale) ||
                              (paintH == bestPaintH && scale == bestScale && fontId == bodyFaceId &&
                               bestFaceId != bodyFaceId) ||
                              (paintH == bestPaintH && scale == bestScale && w < bestW);
          if (!better) return;
          found = true;
          bestPaintH = paintH;
          bestW = w;
          bestScale = scale;
          bestBold = useBold;
          bestFaceId = fontId;
          (void)gTop;
        };

        // Scale 2 first (all faces), then scale 3 only if underfilled.
        for (int i = 0; i < nCand; ++i) {
          tryFaceScale(candidates[i], true, 2);
          tryFaceScale(candidates[i], false, 2);
        }
        if (!found || bestPaintH < maxPaintH * 3 / 4) {
          for (int i = 0; i < nCand; ++i) {
            tryFaceScale(candidates[i], true, 3);
            tryFaceScale(candidates[i], false, 3);
          }
        }
        if (!found) {
          tryFaceScale(bodyFaceId, true, 2);
          tryFaceScale(bodyFaceId, false, 2);
        }

        if (found && bestW >= 8) {
          const int capH = targetH;
          const int gap = std::max(3, bodyEm / 10);
          dropW = std::min(maxW, std::max(16, bestW + gap));
          dropIsRight = false;
          dropBottom = y + capH;
          out.hasDropZone = true;
          out.dropZoneW = static_cast<int16_t>(dropW);
          out.dropZoneH = static_cast<int16_t>(capH);

          // Match body capital tops; keep modest so bottom stays above line-3 words.
          int yAdj = 0;
          int bodyCapH = 0, bodyCapTop = 0;
          if (glyphInkMetrics(renderer, bodyFaceId, EpdFontFamily::REGULAR, letter.c_str(), bodyCapH, bodyCapTop)) {
            const int bodyLead = std::max(0, bodyEm - bodyCapTop);
            yAdj = std::min(bodyLead, bodyLine / 4);
          }
          // If paint + yAdj would poke past bottom of line 2, pull yAdj down.
          if (yAdj + bestPaintH > targetH - 2) {
            yAdj = std::max(0, targetH - 2 - bestPaintH);
          }
          const int capTop = std::max(0, y + yAdj);

          GlyphSpan cap;
          cap.x = 0;
          cap.y = static_cast<int16_t>(capTop);
          cap.fontId = bestFaceId;
          cap.epdStyle = static_cast<uint8_t>((bestBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) |
                                              EpdFontFamily::DROP_CAP);
          cap.dropScale = static_cast<uint8_t>(bestScale);
          cap.text = letter;
          out.spans.push_back(std::move(cap));

          bodyRun = capRun;
          bodyByte = capByte;
          cur.runIndex = bodyRun;
          cur.byteInRun = bodyByte;
          dropCapActive = true;
          dropBodyAlign = Align::Left;
        }
      }
    }

    tokenizeFrom(chapter, block.runBegin, block.runCount, bodyRun, bodyByte, toks);

    int indent = 0;
    // No first-line indent beside drop-cap OR letter float (classic flushes indent).
    const bool besideFloat = dropCapActive || dropW > 0;
    if (!besideFloat && atBlockStart) {
      if (forceUserAlign) {
        // Plain modes: simple first-line indent for Left/Justify only (not CSS).
        if ((userAlign == Align::Left || userAlign == Align::Justify) &&
            block.kind == BlockKind::Paragraph && !dropCapActive) {
          indent = (16 * bodyEm) / 16;  // 1em — common body indent
        }
      } else if ((block.flags & kBlockNoIndent) == 0) {
        // Book's Style: IR indent (default 1em; 0 after heading/hgroup/hr/classes).
        indent = (block.indentEmQ4 * bodyEm) / 16;
      }
    }

    size_t ti = 0;
    bool firstLine = true;
    // While laying body beside the cap, use left align in the float column.
    const Align lineAlign = dropCapActive ? dropBodyAlign : blockAlign;

    // Heading size floor (classic ladder): h1=+2, h2=+1 — measure + paint.
    SizeStep headingFloor = SizeStep::Body;
    if (block.kind == BlockKind::Heading1) headingFloor = SizeStep::Plus2;
    else if (block.kind == BlockKind::Heading2) headingFloor = SizeStep::Plus1;

    while (ti < toks.size()) {
      // Use largest step so mid-heading page breaks leave enough vertical room.
      const int probeLineH = lineH(renderer, baseFontId,
                                   headingFloor > SizeStep::Body ? headingFloor : SizeStep::Body, lc);
      if (y + probeLineH > viewH) {
        // Page full mid-block.
        cur.runIndex = toks[ti].runIndex;
        cur.byteInRun = toks[ti].byteOff;
        out.end = cur;
        out.contentH = static_cast<int16_t>(y);
        out.atChapterEnd = false;
        // Must use the SAME success test as the normal return below. A measure-only
        // pass never emits spans, so testing spans/images alone reported "failed"
        // for every page that fills mid-block — even though out.end advanced. Page
        // maps therefore truncated, chapter page counts came out short, and the
        // last-page walk had to skip whole blocks (99 of 158 on one chapter).
        // Headings tripped it hardest: probeLineH uses the h1/h2 size floor, so a
        // heading near the bottom of a page always overflows the probe.
        return !out.spans.empty() || !out.images.empty() || (out.end != from);
      }

      const int xLeft = xBase(y) + (firstLine ? indent : 0);
      int maxW = widthAt(y) - (firstLine ? indent : 0);
      if (maxW < 16) maxW = 16;

      size_t lineStart = ti;
      int lineW = 0;
      size_t lineEnd = ti;
      // When hyphenation splits a word, we inject prefix/suffix tokens for this line
      // only. Hoisted to the function scope above; clear (not reallocate) per line so
      // the capacity carries over.
      lineToks.clear();
      hyphenPrefixStorage.clear();
      bool usedHyphenSplit = false;
      while (ti < toks.size()) {
        const Tok& t = toks[ti];
        const Run& run = chapter.runs()[t.runIndex];
        SizeStep step = run.sizeStep;
        if (static_cast<int>(step) < static_cast<int>(headingFloor)) step = headingFloor;
        const int fid = FontLadder::resolve(baseFontId, step);
        const auto st = static_cast<EpdFontFamily::Style>(FontLadder::epdStyleBits(run.style));
        int tw = 0;
        if (t.space) {
          // Flanking word codepoints for kerned gap (classic ParsedText path).
          uint32_t leftCp = 0, rightCp = 0;
          if (ti > 0 && !toks[ti - 1].space) leftCp = tokenLastCp(chapter, toks[ti - 1]);
          if (ti + 1 < toks.size() && !toks[ti + 1].space) rightCp = tokenFirstCp(chapter, toks[ti + 1]);
          tw = measureInterWordSpace(renderer, fid, st, leftCp, rightCp);
        } else if (t.w >= 0) {
          tw = t.w;  // already measured (previous line attempt, or an earlier pass)
        } else {
          tw = measureWord(renderer, fid, st, chapter.runText(run) + t.byteOff, t.byteLen);
          if (tw >= 0 && tw <= INT16_MAX) toks[ti].w = static_cast<int16_t>(tw);
        }
        if (lineW + tw > maxW && lineEnd > lineStart) {
          // Keep "." / "," / "!" with the preceding word (slight overflow ok).
          if (!t.space && tokenIsOnlyClosingPunct(chapter, t)) {
            lineW += tw;
            lineEnd = ti + 1;
            ++ti;
            break;
          }
          // Word does not fit: hyphenate only if allowed; otherwise whole word → next line.
          if (!t.space && t.byteLen > 1) {
            const int remain = maxW - lineW;
            size_t prefBytes = 0;
            int prefW = -1;
            if (tryHyphenateWordToFit(renderer, fid, st, chapter.runText(run) + t.byteOff, t.byteLen, remain,
                                      hyphenationEnabled, prefBytes, hyphenPrefixStorage, prefW) &&
                prefBytes > 0 && prefBytes < t.byteLen) {
              lineToks.assign(toks.begin() + static_cast<std::ptrdiff_t>(lineStart),
                              toks.begin() + static_cast<std::ptrdiff_t>(lineEnd));
              Tok prefixTok = t;
              prefixTok.byteOff = 0;
              prefixTok.byteLen = static_cast<uint16_t>(hyphenPrefixStorage.size());
              prefixTok.space = false;
              // Prefix text lives in hyphenPrefixStorage, not the run; prefW is its
              // measured width, so the emit loop can reuse it directly.
              prefixTok.w = (prefW >= 0 && prefW <= INT16_MAX) ? static_cast<int16_t>(prefW) : -1;
              lineToks.push_back(prefixTok);
              lineW += prefW;
              usedHyphenSplit = true;
              toks[ti].byteOff = static_cast<uint16_t>(t.byteOff + prefBytes);
              toks[ti].byteLen = static_cast<uint16_t>(t.byteLen - prefBytes);
              toks[ti].w = -1;  // token now covers only the suffix — remeasure
              break;
            }
          }
          // No mid-word chop — end this line; word starts the next line whole.
          break;
        }
        if (lineW + tw > maxW && lineEnd == lineStart) {
          // Whole line for this word. Hyphenate only (hard hyphens / setting on).
          // Never force-split letters; if still too wide, emit the whole word.
          if (!t.space && t.byteLen > 1) {
            size_t prefBytes = 0;
            int prefW = -1;
            if (tryHyphenateWordToFit(renderer, fid, st, chapter.runText(run) + t.byteOff, t.byteLen, maxW,
                                      hyphenationEnabled, prefBytes, hyphenPrefixStorage, prefW) &&
                prefBytes > 0 && prefBytes < t.byteLen) {
              Tok prefixTok = t;
              prefixTok.byteOff = 0;
              prefixTok.byteLen = static_cast<uint16_t>(hyphenPrefixStorage.size());
              prefixTok.w = (prefW >= 0 && prefW <= INT16_MAX) ? static_cast<int16_t>(prefW) : -1;
              lineToks.clear();
              lineToks.push_back(prefixTok);
              lineW = prefW;
              usedHyphenSplit = true;
              toks[ti].byteOff = static_cast<uint16_t>(t.byteOff + prefBytes);
              toks[ti].byteLen = static_cast<uint16_t>(t.byteLen - prefBytes);
              toks[ti].w = -1;  // token now covers only the suffix — remeasure
              lineEnd = lineStart;
              break;
            }
          }
          // Whole word on this line (may extend slightly past the measure width).
          lineW += tw;
          lineEnd = ti + 1;
          ++ti;
          break;
        }
        lineW += tw;
        lineEnd = ti + 1;
        ++ti;
      }

      // Source of tokens for this line (hyphen path materializes into lineToks).
      const std::vector<Tok>* emit = &toks;
      size_t emitBegin = lineStart;
      size_t emitEnd = lineEnd;
      if (usedHyphenSplit && !lineToks.empty()) {
        emit = &lineToks;
        emitBegin = 0;
        emitEnd = lineToks.size();
      }

      int shift = 0;
      if (lineAlign == Align::Center) shift = std::max(0, (maxW - lineW) / 2);
      if (lineAlign == Align::Right) shift = std::max(0, maxW - lineW);

      int justifyExtra = 0;
      int spaces = 0;
      // No justify while still wrapping beside the drop-cap float.
      const bool inDropFloat = dropW > 0 && y < dropBottom;
      if (lineAlign == Align::Justify && !inDropFloat && ti < toks.size()) {
        for (size_t k = emitBegin; k < emitEnd; ++k) {
          if ((*emit)[k].space) ++spaces;
        }
        justifyExtra = std::max(0, maxW - lineW);
      }

      // Line height follows the largest size step on the line (headings, etc.).
      SizeStep lineStep = headingFloor;
      int maxAscOnLine = 0;
      for (size_t k = emitBegin; k < emitEnd; ++k) {
        if (!(*emit)[k].space) {
          SizeStep rs = chapter.runs()[(*emit)[k].runIndex].sizeStep;
          if (static_cast<int>(rs) < static_cast<int>(headingFloor)) rs = headingFloor;
          if (static_cast<int>(rs) > static_cast<int>(lineStep)) lineStep = rs;
          const int fid = FontLadder::resolve(baseFontId, rs);
          maxAscOnLine = std::max(maxAscOnLine, renderer.getFontAscenderSize(fid));
        }
      }
      const int thisLineH = lineH(renderer, baseFontId, lineStep, lc);
      if (maxAscOnLine < 8) maxAscOnLine = std::max(8, renderer.getFontAscenderSize(baseFontId));
      const int lineBoxTop = y;

      int x = xLeft + shift;
      int spacesLeft = spaces;
      for (size_t k = emitBegin; k < emitEnd; ++k) {
        const Tok& t = (*emit)[k];
        const Run& run = chapter.runs()[t.runIndex];
        SizeStep step = run.sizeStep;
        if (static_cast<int>(step) < static_cast<int>(headingFloor)) step = headingFloor;
        const int fid = FontLadder::resolve(baseFontId, step);
        const auto st = static_cast<EpdFontFamily::Style>(FontLadder::epdStyleBits(run.style));
        if (t.space) {
          uint32_t leftCp = 0, rightCp = 0;
          // Prefer real neighbors on the emit line (skip synthetic hyphen edge cases).
          if (k > emitBegin && !(*emit)[k - 1].space) leftCp = tokenLastCp(chapter, (*emit)[k - 1]);
          if (k + 1 < emitEnd && !(*emit)[k + 1].space) rightCp = tokenFirstCp(chapter, (*emit)[k + 1]);
          // Hyphen prefix line: last word may be synthetic — fall back to source toks.
          if (leftCp == 0 && usedHyphenSplit && lineEnd > lineStart) {
            // previous non-space on source line
            for (size_t j = lineEnd; j > lineStart;) {
              --j;
              if (!toks[j].space) {
                leftCp = tokenLastCp(chapter, toks[j]);
                break;
              }
            }
          }
          int sw = measureInterWordSpace(renderer, fid, st, leftCp, rightCp);
          if (spacesLeft > 0 && justifyExtra > 0) {
            const int add = justifyExtra / spacesLeft;
            sw += add;
            justifyExtra -= add;
            --spacesLeft;
          }
          x += sw;
          continue;
        }
        // Hyphen prefix lives in hyphenPrefixStorage (last emit token when split).
        const bool isHyphenPrefix = usedHyphenSplit && k + 1 == emitEnd && !hyphenPrefixStorage.empty();
        const char* tokBytes = isHyphenPrefix ? hyphenPrefixStorage.data() : chapter.runText(run) + t.byteOff;
        const size_t tokLen = isHyphenPrefix ? hyphenPrefixStorage.size() : t.byteLen;

        // Reuse the width the fitting loop already measured for this exact
        // (fontId, style, bytes) triple — see Tok::w. Falls back to measuring
        // only when the token was never measured (defensive; should not happen).
        const int advance = (t.w >= 0) ? static_cast<int>(t.w) : measureWord(renderer, fid, st, tokBytes, tokLen);

        if (!params.measureOnly) {
          // Shared baseline on the line: drawText baseline = y + ascender(fid).
          // Set y so all spans share baseline = lineBoxTop + maxAscOnLine.
          const int spanAsc = std::max(1, renderer.getFontAscenderSize(fid));
          const int paintTop = lineBoxTop + maxAscOnLine - spanAsc;
          GlyphSpan sp;
          sp.x = static_cast<int16_t>(x);
          sp.y = static_cast<int16_t>(std::max(0, paintTop));
          sp.fontId = fid;
          sp.epdStyle = FontLadder::epdStyleBits(run.style);
          sp.text.assign(tokBytes, tokLen);
          out.spans.push_back(std::move(sp));
        }
        x += advance;
      }

      y += thisLineH;
      firstLine = false;
      indent = 0;
    }

    // Finished block content — minimal extra after headings (classic is dense).
    y += marginBottom;
    // Text/images break spacer margin collapse chain (non-zero content).
    trailingCollapsePx = 0;
    if (isHeading && block.kind == BlockKind::Heading1) {
      y += bodyLine / 6;
    }
    // Drop-cap text block: after its own lines, jump past the 2-line float.
    // Letter/figure floats stay active so following paragraphs keep wrapping.
    if (dropW > 0 && dropCapActive && y < dropBottom) {
      y = dropBottom + std::max(2, bodyEm / 8);
      dropW = 0;
      dropBottom = 0;
      dropIsRight = false;
    } else if (dropW > 0 && y >= dropBottom) {
      dropW = 0;
      dropBottom = 0;
      dropIsRight = false;
    }
    advancePastBlock(chapter, cur);
  }

  out.end = cur;
  out.contentH = static_cast<int16_t>(y);
  out.atChapterEnd = atEnd(chapter, cur);
  // Valid page if we painted anything, finished the chapter, OR advanced the
  // cursor (spacer-only / empty-run bands). Returning false on "progress but no
  // spans" made nextPage fail mid-chapter; turnNext then jumped to the next
  // spine and chapters looked like they were only 2–3 pages long.
  return !out.spans.empty() || !out.images.empty() || out.atChapterEnd || (out.end != from);
}

bool PageLayouter::buildFullPageMap(const ChapterIr& chapter, const GfxRenderer& renderer, const LayoutParams& params,
                                    PageMap& map) {
  map.clear();
  map.setRenderKey(params.key);
  if (chapter.empty()) {
    map.markComplete(0);
    return true;
  }
  IrCursor cur{};
  cur.blockIndex = 0;
  cur.runIndex = chapter.blocks()[0].runBegin;
  cur.byteInRun = 0;
  map.resetWithStart(cur);

  LaidOutPage page;
  // A single unlayoutable block (broken image, empty cell) must not truncate the
  // whole map — skip past it instead of returning an incomplete 1-2 page map.
  int skips = 0;
  // One skip per block: the walk never revisits a block, so this cannot spin, and
  // a flat cap truncated long chapters partway through (see goToLastPage).
  const int kMaxBlockSkips = static_cast<int>(chapter.blocks().size()) + 8;
  for (int guard = 0; guard < 20000; ++guard) {
    if (!layoutPage(chapter, renderer, params, cur, page)) {
      if (!page.atChapterEnd && cur.blockIndex + 1 < chapter.blocks().size() && ++skips <= kMaxBlockSkips) {
        ++cur.blockIndex;
        cur.runIndex = chapter.blocks()[cur.blockIndex].runBegin;
        cur.byteInRun = 0;
        map.setPageStart(map.knownPages() > 0 ? map.knownPages() - 1 : 0, cur);
        continue;
      }
      // Failed layout is not a chapter end — leave incomplete.
      return map.knownPages() > 0;
    }
    if (page.atChapterEnd) {
      const int est = chapter.estimatePageCount(params.key.viewportW, params.key.viewportH, params.bodyEmPx,
                                                params.lineCompression);
      // Only refuse absurd 1–3 page "completes" for large chapters — not honest maps
      // that are merely shorter than a padded estimate.
      if (!(est >= 10 && map.knownPages() <= 3)) {
        map.markComplete(map.knownPages());
      }
      return true;
    }
    if (page.end.blockIndex == cur.blockIndex && page.end.runIndex == cur.runIndex &&
        page.end.byteInRun == cur.byteInRun) {
      // Stuck: do not mark complete (that produced 2–3 page false totals).
      return map.knownPages() > 0;
    }
    cur = page.end;
    map.pushPageStart(cur);
  }
  // Guard exhausted without atChapterEnd — incomplete.
  return map.knownPages() > 0;
}

}  // namespace rivulet
