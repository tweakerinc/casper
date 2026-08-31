#pragma once

#include "EpdFontData.h"

/// Synthetic ♩♪♫♬ when a face has no native Miscellaneous Symbols coverage.
///
/// Builtin Literata / Source Serif omit U+2669–U+266C, and they also omit
/// U+FFFD, so getGlyph() used to return nullptr and layout advanced 0 px.
/// Project Hail Mary then renders Rocky's chord-speech as `“, ” he says.`
/// (issue #11): the notes vanish, the grammatical comma stays.
///
/// Bitmaps live in flash as uncompressed 1-bit / 2-bit packs. They are not
/// members of EpdFontData::glyph, so GfxRenderer::getGlyphBitmap must return
/// bitmapIfSynthetic() *before* treating the pointer as a compressed-group
/// index (glyph - fontData->glyph would be nonsense).
namespace musicNoteFallback {

constexpr uint32_t FIRST = 0x2669;  // ♩
constexpr uint32_t LAST = 0x266C;   // ♬

constexpr bool covers(const uint32_t cp) { return cp >= FIRST && cp <= LAST; }

const EpdGlyph* glyph(uint32_t cp);

/// Uncompressed bitmap for a synthetic glyph, packed to match `is2Bit`.
/// Returns nullptr when `glyph` is not one of ours.
const uint8_t* bitmapIfSynthetic(const EpdGlyph* glyph, bool is2Bit);

}  // namespace musicNoteFallback
