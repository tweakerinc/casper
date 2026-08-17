#pragma once

#include <stdint.h>

// 8x8 Bayer ordered dither (values 0..63). Larger matrix than 4x4 reduces
// visible cross-hatch on e-ink photos and line art after 4-level quantize.
inline const uint8_t bayer8x8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

// Legacy 4x4 kept for any caller that still wants the lighter kernel.
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Tone path for reader image decode (Bayer 4-level — no Atkinson error buffers).
// Cover gen uses Atkinson + multipass; here we approximate cover midtones cheaply.
enum class EinkImageTone : uint8_t {
  Plate = 0,  // photos / full art — lift midtones (avoid black-box BW)
  Ink = 1,    // drop-cap / thin letter glyphs — darken midtones
  Cover = 2,  // document text-in-JPEG (Illuminae briefings) — cover-thumb curve
};

// Pull midtones toward black so Tenniel/woodcut *letter* strokes and drop-cap
// images stay as dark as the printed book. Mild linear mix with a square term
// (approx gamma > 1 on the ink axis) without crushing pure paper white.
//
// Do NOT use on full illustration plates: Alice p0028-style shaded inserts
// average ~120 gray and this map turns ~60% of pixels pure black (99% solid
// black on the BW pass) — the classic "black box" look.
inline uint8_t darkenForEinkInk(const uint8_t gray) {
  const int g = static_cast<int>(gray);
  const int sq = (g * g) / 255;
  // ~35% linear + 65% squared → darkens grays, leaves 0 and ~255 near ends
  int out = (g * 45 + sq * 83) / 128;
  if (out < 0) out = 0;
  if (out > 255) out = 255;
  return static_cast<uint8_t>(out);
}

// Mild paper lift for full plates / photos (Tenniel, woodcuts, chapter art).
// Strong lift (gain 1.30 + bias 28) washed Alice/DCC plates to "a little light"
// on e-ink. Keep a gentle open so midtones are not a solid black box, without
// bleaching hatching. Cheap integer gain+bias only — no histogram buffers.
inline uint8_t liftForEinkPlate(const uint8_t gray) {
  // ≈ gain 1.10 + bias 10 → mean 120 becomes ~142 (was ~184).
  int g = (static_cast<int>(gray) * 141 + 10 * 128) / 128;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  return static_cast<uint8_t>(g);
}

// Same mild open as BitmapHelpers::adjustPixelCoverThumb / adjustPixel1Bit
// (home cover gen). Soft gamma + small lift — readable type without ink crush.
// Used for document plates (Illuminae briefings) with Bayer, not Atkinson.
inline uint8_t toneMapCoverDoc(const uint8_t gray) {
  int g = static_cast<int>(gray);
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  const int product = g * 255;
  int root = g;
  if (root > 0) {
    root = (root + product / root) >> 1;
    root = (root + product / root) >> 1;
  }
  // ~20% toward sqrt + lift 8 (matches cover thumb)
  int adjusted = (g * 80 + root * 20) / 100 + 8;
  // Very light cream nudge only (no hard white rail).
  if (adjusted > 155 && adjusted < 230) {
    adjusted += 6;
  }
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return static_cast<uint8_t>(adjusted);
}

// Ink-biased breakpoints for letter glyphs / pure line art on white paper.
//   0 black | 1 dark gray | 2 light gray | 3 white
inline uint8_t quantizeGray4LevelInk(int adjusted) {
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  if (adjusted < 96) return 0;
  if (adjusted < 155) return 1;
  if (adjusted < 210) return 2;
  return 3;
}

// Plate quantize: slightly ink-leaning vs pure paper open so Tenniel hatching
// stays visible after mild lift. BW paints levels 0–2 black; greys recover 1–2.
inline uint8_t quantizeGray4LevelNeutral(int adjusted) {
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  if (adjusted < 32) return 0;   // deep ink
  if (adjusted < 95) return 1;   // dark gray
  if (adjusted < 165) return 2;  // light gray structure
  return 3;                      // paper / highlights
}

// Equal 0/85/170/255 bins — same as home-cover Atkinson balancedLevels.
// Keeps midtones mid after cover-thumb tone map (not crushed to dark).
inline uint8_t quantizeGray4LevelCover(int adjusted) {
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Apply 8x8 Bayer dither and quantize to 4 levels (0-3).
// Stateless — safe for MCU/scanline streaming in any order.
// Prefer Cover for text-in-JPEG; Ink for drop-caps; Plate for photos/art.
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y, EinkImageTone tone = EinkImageTone::Plate) {
  if (tone == EinkImageTone::Ink) {
    gray = darkenForEinkInk(gray);
  } else if (tone == EinkImageTone::Cover) {
    gray = toneMapCoverDoc(gray);
  } else {
    gray = liftForEinkPlate(gray);
  }
  const int bayer = bayer8x8[y & 7][x & 7];
  // Plate: moderate dither (was ±20 — grainy on woodcuts). Cover mid. Ink tight.
  int dither;
  if (tone == EinkImageTone::Ink) {
    dither = (bayer - 32) / 2;
  } else if (tone == EinkImageTone::Cover) {
    dither = ((bayer - 32) * 3) / 8;  // ±12-ish
  } else {
    dither = ((bayer - 32) * 3) / 8;  // ±12 — less Bayer grain on Tenniel/plates
  }
  const int adjusted = static_cast<int>(gray) + dither;
  if (tone == EinkImageTone::Ink) return quantizeGray4LevelInk(adjusted);
  if (tone == EinkImageTone::Cover) return quantizeGray4LevelCover(adjusted);
  return quantizeGray4LevelNeutral(adjusted);
}

// Point quantize without dither (performance / solid fills).
inline uint8_t quantizeGray4LevelNoDither(uint8_t gray, EinkImageTone tone = EinkImageTone::Plate) {
  if (tone == EinkImageTone::Ink) {
    gray = darkenForEinkInk(gray);
    return quantizeGray4LevelInk(static_cast<int>(gray));
  }
  if (tone == EinkImageTone::Cover) {
    gray = toneMapCoverDoc(gray);
    return quantizeGray4LevelCover(static_cast<int>(gray));
  }
  gray = liftForEinkPlate(gray);
  return quantizeGray4LevelNeutral(static_cast<int>(gray));
}

// Back-compat: bool inkBias → Plate/Ink (Cover not expressible).
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y, bool inkBias) {
  return applyBayerDither4Level(gray, x, y, inkBias ? EinkImageTone::Ink : EinkImageTone::Plate);
}
inline uint8_t quantizeGray4LevelNoDither(uint8_t gray, bool inkBias) {
  return quantizeGray4LevelNoDither(gray, inkBias ? EinkImageTone::Ink : EinkImageTone::Plate);
}

// Back-compat aliases (old single-path names).
inline uint8_t quantizeGray4Level(int adjusted) { return quantizeGray4LevelInk(adjusted); }
