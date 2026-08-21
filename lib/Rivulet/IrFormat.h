#pragma once

#include <cstdint>

// On-disk chapter IR (Tier B). Bump kIrFormatVersion when layout changes.
namespace rivulet {

inline constexpr char kIrMagic[4] = {'R', 'V', 'I', 'R'};
// v22: keep a leading space after style runs (</i> / </b> / </span>) so
// "Vampire"+" skill" is not glued when HTML whitespace was the only separator.
// v21 IR may already have glued runs; reconvert to pick this up.
// Layout used to synthesize a gap between adjacent word-char runs as a backstop
// for stripped spaces. That gap became justify glue, so ordinal suffixes
// ("6" + superscript "th") stretched to the right margin. Adjacent word-char
// runs are now left glued; v23 IR already stores the real leading space when
// HTML had one.
// v21: table/tr/td/th are parsed — each cell becomes its own block. v20 IR was
// produced by a parser that did not know these tags, so its cell text is already
// merged into run-on paragraphs and cannot be recovered without reconverting.
// v20: RunStyle carries text decorations (underline / strikethrough / sup / sub).
// v23: real typography preserved. v22 and earlier flattened curly quotes to ' ",
// en/em dashes to -, and the ellipsis to "..." while building the IR, so cached
// chapters hold the flattened text and must be reconverted to get it back.
inline constexpr uint16_t kIrFormatVersion = 23;
// Accept this version on load (inclusive range).
inline constexpr uint16_t kIrFormatVersionMin = 23;
inline constexpr uint16_t kIrFormatVersionMax = 23;

// Render-spec fingerprint: layout maps invalid when this changes.
struct RenderKey {
  int32_t fontId = 0;
  uint16_t viewportW = 0;
  uint16_t viewportH = 0;
  uint8_t marginL = 0;
  uint8_t marginR = 0;
  uint8_t marginT = 0;
  uint8_t marginB = 0;
  uint16_t lineCompressionQ8 = 256;  // 1.0 = 256 (8.8 fixed)
  // bit0 = Book's Style (honor IR CSS align/spacers). Off = force L/C/R/J (bits 2–3).
  // bit1 = extra paragraph spacing on. bits 2–3 = force align when bit0 clear.
  // bit4 0x10 = hyphenation (layout). bit5 0x20 = focus reading (paint). bit6 0x40 = guide dots (paint).
  // bit7 0x80 = full-line extra paragraph gap (legacy; with bit1). Prefer pad[5:4] when set.
  // Focus + guide apply only when bit0 is clear (forced L/C/R/J) so Book's Style stays clean.
  uint8_t flags = 0;
  // Low nibble: imageRendering (0–2). Bits 5:4: extra-para height when bit1 set —
  // 0 = half (or legacy bit7), 1 = full, 2 = quarter. High bits reserved.
  uint8_t pad = 0;

  bool operator==(const RenderKey& o) const {
    return fontId == o.fontId && viewportW == o.viewportW && viewportH == o.viewportH && marginL == o.marginL &&
           marginR == o.marginR && marginT == o.marginT && marginB == o.marginB &&
           lineCompressionQ8 == o.lineCompressionQ8 && flags == o.flags;
  }
  bool operator!=(const RenderKey& o) const { return !(*this == o); }
};

// Per-run face + decoration bits.
//
// These values deliberately mirror EpdFontFamily::Style one-for-one, so
// FontLadder::epdStyleBits() is a straight cast and the two can never drift
// (a static_assert in FontLadder.cpp enforces it). Treat this as a BITMASK:
// decorations compose freely with bold/italic, e.g. Bold|Underline.
//
// Before v20 only the low two bits existed and the parser threw away every
// <sup>/<sub>/<u>/<s>/<del>/<ins>, so footnote markers rendered full-size on the
// baseline and struck-out text lost its meaning.
enum class RunStyle : uint8_t {
  Regular = 0,
  Bold = 1,
  Italic = 2,
  BoldItalic = 3,
  Underline = 4,
  Strikethrough = 8,
  Superscript = 16,
  Subscript = 32,
};

inline constexpr uint8_t kRunStyleFaceMask = 0x03;        // bold | italic
inline constexpr uint8_t kRunStyleDecorationMask = 0x0C;  // underline | strikethrough
inline constexpr uint8_t kRunStyleScriptMask = 0x30;      // sup | sub

inline constexpr RunStyle operator|(RunStyle a, RunStyle b) {
  return static_cast<RunStyle>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline constexpr RunStyle& operator|=(RunStyle& a, RunStyle b) {
  a = a | b;
  return a;
}
inline constexpr bool hasStyleBit(RunStyle s, RunStyle bit) {
  return (static_cast<uint8_t>(s) & static_cast<uint8_t>(bit)) != 0;
}

// Relative size vs user base face. 0 = two steps smaller … 4 = two steps larger.
// 2 = body (SIZE_STEP_BASE).
enum class SizeStep : int8_t {
  Minus2 = 0,
  Minus1 = 1,
  Body = 2,
  Plus1 = 3,
  Plus2 = 4,
};

enum class Align : uint8_t {
  Left = 0,
  Center = 1,
  Right = 2,
  Justify = 3,
};

enum class BlockKind : uint8_t {
  Paragraph = 0,
  Heading1 = 1,
  Heading2 = 2,
  Heading3 = 3,
  Heading4 = 4,
  Heading5 = 5,
  Heading6 = 6,
  HorizontalRule = 7,
  Spacer = 8,  // empty vertical gap (e.g. br-br)
  Image = 9,   // run text = EPUB-relative href; imageW/H = layout size in px
};

// Block flags.
inline constexpr uint16_t kBlockDropCap = 1u << 0;         // first letter floats as drop cap
inline constexpr uint16_t kBlockNoIndent = 1u << 1;        // suppress auto first-line indent
inline constexpr uint16_t kBlockForcePageBreak = 1u << 2;  // start on new page if possible
inline constexpr uint16_t kBlockFloatLeft = 1u << 3;       // image: left float + wrap (figleft / letter)
inline constexpr uint16_t kBlockFloatRight = 1u << 4;      // image: right float + wrap
inline constexpr uint16_t kBlockOrnament = 1u << 5;        // small chapter ornament (e.g. .orn img ~12% width)

// Page map magic
inline constexpr char kMapMagic[4] = {'R', 'V', 'P', 'M'};
inline constexpr uint16_t kMapFormatVersion = 1;

}  // namespace rivulet
