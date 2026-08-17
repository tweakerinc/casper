#pragma once

#include <cstdint>

// On-disk chapter IR (Tier B). Bump kIrFormatVersion when layout changes.
namespace rivulet {

inline constexpr char kIrMagic[4] = {'R', 'V', 'I', 'R'};
// v19: force reconvert of v18 chapter IR that predates reliable Image blocks
// (Fourth Wing .orn ornaments, Alice floats). Same on-disk layout as v18.
// v18: HTML lists/nav (ol/ul/li) each li = paragraph — Contents pages no longer
// collapse into one run-on paragraph (Isako TOC). Also v17 document-alt text.
inline constexpr uint16_t kIrFormatVersion = 19;
// Accept this version on load (inclusive range).
inline constexpr uint16_t kIrFormatVersionMin = 19;
inline constexpr uint16_t kIrFormatVersionMax = 19;

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

// Per-run face bits (Latin v1 — no BiDi/RTL).
enum class RunStyle : uint8_t {
  Regular = 0,
  Bold = 1,
  Italic = 2,
  BoldItalic = 3,
};

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
inline constexpr uint16_t kBlockDropCap = 1u << 0;       // first letter floats as drop cap
inline constexpr uint16_t kBlockNoIndent = 1u << 1;      // suppress auto first-line indent
inline constexpr uint16_t kBlockForcePageBreak = 1u << 2;  // start on new page if possible
inline constexpr uint16_t kBlockFloatLeft = 1u << 3;     // image: left float + wrap (figleft / letter)
inline constexpr uint16_t kBlockFloatRight = 1u << 4;    // image: right float + wrap
inline constexpr uint16_t kBlockOrnament = 1u << 5;      // small chapter ornament (e.g. .orn img ~12% width)

// Page map magic
inline constexpr char kMapMagic[4] = {'R', 'V', 'P', 'M'};
inline constexpr uint16_t kMapFormatVersion = 1;

}  // namespace rivulet
