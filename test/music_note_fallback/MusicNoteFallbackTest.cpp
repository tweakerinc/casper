#include <gtest/gtest.h>

#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"
#include "lib/EpdFont/MusicNoteFallback.h"

// Builtin Literata/Source Serif skip U+2669–U+266C and U+FFFD, so a miss
// used to return nullptr and advance 0 — Hail Mary chord-speech collapsed
// to `“, ” he says.` (issue #11). These tests pin the synthetic fallback.

namespace {

const EpdGlyph kGlyphs[] = {
    {7, 11, 173, 2, 5, 0, 0},   // ','  ~10.8 px, same order as literata 16
    {8, 12, 137, 0, 12, 0, 0},  // 'T'
};

const EpdUnicodeInterval kIntervals[] = {
    {0x002C, 0x002C, 0},
    {0x0054, 0x0054, 1},
};

const EpdFontData kNoTofuFont = {
    .bitmap = nullptr,
    .glyph = kGlyphs,
    .intervals = kIntervals,
    .intervalCount = 2,
    .advanceY = 16,
    .ascender = 12,
    .descender = 0,
    .is2Bit = true,
    .groups = nullptr,
    .groupCount = 0,
    .glyphToGroup = nullptr,
    .kernLeftClasses = nullptr,
    .kernRightClasses = nullptr,
    .kernMatrix = nullptr,
    .kernLeftEntryCount = 0,
    .kernRightEntryCount = 0,
    .kernLeftClassCount = 0,
    .kernRightClassCount = 0,
    .ligaturePairs = nullptr,
    .ligaturePairCount = 0,
    .glyphMissHandler = nullptr,
    .glyphMissCtx = nullptr,
    .coverageHandler = nullptr,
};

int inkPixels2Bit(const EpdGlyph* glyph) {
  const uint8_t* bits = musicNoteFallback::bitmapIfSynthetic(glyph, true);
  if (!bits) return 0;
  int ink = 0;
  const int n = static_cast<int>(glyph->width) * static_cast<int>(glyph->height);
  for (int pos = 0; pos < n; ++pos) {
    const uint8_t byte = bits[pos >> 2];
    const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
    if (raw == 3) ++ink;
  }
  return ink;
}

}  // namespace

TEST(MusicNoteFallback, CoversTheFourStaffNotesOnly) {
  EXPECT_FALSE(musicNoteFallback::covers(0x2668));
  EXPECT_TRUE(musicNoteFallback::covers(0x2669));
  EXPECT_TRUE(musicNoteFallback::covers(0x266A));
  EXPECT_TRUE(musicNoteFallback::covers(0x266B));
  EXPECT_TRUE(musicNoteFallback::covers(0x266C));
  EXPECT_FALSE(musicNoteFallback::covers(0x266D));
  EXPECT_FALSE(musicNoteFallback::covers(','));
}

TEST(MusicNoteFallback, EachNoteHasInkAndAdvance) {
  for (uint32_t cp = musicNoteFallback::FIRST; cp <= musicNoteFallback::LAST; ++cp) {
    const EpdGlyph* g = musicNoteFallback::glyph(cp);
    ASSERT_NE(g, nullptr) << cp;
    EXPECT_GT(g->width, 0);
    EXPECT_GT(g->height, 0);
    EXPECT_GT(g->advanceX, 0);
    EXPECT_GT(inkPixels2Bit(g), 20) << "note U+" << std::hex << cp << " is blank";
    EXPECT_NE(musicNoteFallback::bitmapIfSynthetic(g, true), nullptr);
    EXPECT_NE(musicNoteFallback::bitmapIfSynthetic(g, false), nullptr);
  }
}

TEST(MusicNoteFallback, ForeignGlyphPointerIsNotSynthetic) {
  EXPECT_EQ(musicNoteFallback::bitmapIfSynthetic(&kGlyphs[0], true), nullptr);
  EXPECT_EQ(musicNoteFallback::glyph(','), nullptr);
}

TEST(MusicNoteFallback, GetGlyphDoesNotCollapseNotesToNull) {
  const EpdFont font(&kNoTofuFont);
  EXPECT_EQ(font.getGlyph(','), &kGlyphs[0]);
  EXPECT_EQ(font.getGlyph('T'), &kGlyphs[1]);
  EXPECT_EQ(font.getGlyph('x'), nullptr) << "unknown still misses when FFFD is absent";

  const EpdGlyph* eighth = font.getGlyph(0x266A);
  ASSERT_NE(eighth, nullptr);
  EXPECT_EQ(eighth, musicNoteFallback::glyph(0x266A));
  EXPECT_TRUE(font.hasCodepoint(0x266A));
  EXPECT_TRUE(font.hasCodepoint(0x266B));
  EXPECT_FALSE(font.hasCodepoint('x'));
  EXPECT_TRUE(font.hasCodepoint(','));
}

TEST(MusicNoteFallback, HailMaryChordKeepsWidthUnlikeBareComma) {
  // Screenshot of issue #11: `“, ” he says.` — notes vanished, comma stayed.
  const EpdFont font(&kNoTofuFont);
  int commaW = 0, commaH = 0;
  int chordW = 0, chordH = 0;
  font.getTextDimensions(",", &commaW, &commaH);
  font.getTextDimensions("\xE2\x99\xAB\xE2\x99\xAB\xE2\x99\xAB,", &chordW, &chordH);
  EXPECT_GT(commaW, 0);
  EXPECT_GT(chordW, commaW * 3) << "three beamed notes + comma must not collapse onto the comma";
}
