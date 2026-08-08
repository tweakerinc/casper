#pragma once

#include <stdint.h>

// 8x8 Bayer ordered dither (values 0..63). Larger matrix than 4x4 reduces
// visible cross-hatch on e-ink photos and line art after 4-level quantize.
inline const uint8_t bayer8x8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
};

// Legacy 4x4 kept for any caller that still wants the lighter kernel.
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
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

// Paper lift for full plates / photos (Bare-cover-like midtones).
// Scanned art and full-page JPEGs (e.g. DCC inter-chapter plates ~1200×1727)
// sit around mean ~120; without lift the BW plane paints most of the figure
// solid black and greys only partially recover — "hard to see" on e-ink.
// Cheap integer gain+bias only — no histogram / no extra buffers.
inline uint8_t liftForEinkPlate(const uint8_t gray) {
  // ≈ gain 1.30 + bias 28 → mean 120 becomes ~184 (more paper / light gray).
  int g = (static_cast<int>(gray) * 166 + 28 * 128) / 128;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  return static_cast<uint8_t>(g);
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

// Paper-leaning breakpoints for full plates (cover multipass quality on greys,
// and a readable BW fallback when greys are deferred under heap pressure).
// Narrow pure-black band; generous white so hatching is not a solid blob.
inline uint8_t quantizeGray4LevelNeutral(int adjusted) {
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  if (adjusted < 22) return 0;   // deep ink only
  if (adjusted < 78) return 1;   // dark gray
  if (adjusted < 155) return 2;  // light gray (wider — more structure on plates)
  return 3;                     // paper / highlights
}

// Apply 8x8 Bayer dither and quantize to 4 levels (0-3).
// inkBias=true: drop-cap / thin stroke glyphs. inkBias=false: full figures.
// Stateless — safe for MCU/scanline streaming in any order.
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y, bool inkBias = false) {
  if (inkBias) {
    gray = darkenForEinkInk(gray);
  } else {
    gray = liftForEinkPlate(gray);
  }
  const int bayer = bayer8x8[y & 7][x & 7];
  // Plates: slightly wider dither (±20) so hatching survives lift+quantize.
  // Glyphs: tighter (±16) so thin strokes are not speckled away.
  const int dither = inkBias ? ((bayer - 32) / 2) : (((bayer - 32) * 5) / 8);
  const int adjusted = static_cast<int>(gray) + dither;
  return inkBias ? quantizeGray4LevelInk(adjusted) : quantizeGray4LevelNeutral(adjusted);
}

// Point quantize without dither (performance / solid fills).
inline uint8_t quantizeGray4LevelNoDither(uint8_t gray, bool inkBias = false) {
  if (inkBias) {
    gray = darkenForEinkInk(gray);
    return quantizeGray4LevelInk(static_cast<int>(gray));
  }
  gray = liftForEinkPlate(gray);
  return quantizeGray4LevelNeutral(static_cast<int>(gray));
}

// Back-compat aliases (old single-path names). Prefer applyBayerDither4Level(..., inkBias).
inline uint8_t quantizeGray4Level(int adjusted) { return quantizeGray4LevelInk(adjusted); }
