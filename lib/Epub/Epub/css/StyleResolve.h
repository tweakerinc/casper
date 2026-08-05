#pragma once

#include <cstdint>

#include "CssStyle.h"

class GfxRenderer;

// Rivulet Layout Core — relative font size steps and line-height resolution.
// sizeStep is relative to the user-chosen base face (SIZE_STEP_BASE), not a fontId.

static constexpr uint8_t SIZE_STEP_BASE = 2;  // maps to user-chosen size
static constexpr uint8_t SIZE_STEP_MIN = 0;   // two steps smaller than user
static constexpr uint8_t SIZE_STEP_MAX = 4;   // two steps larger

struct StyleResolveContext {
  int baseFontId = 0;
  float baseEmPx = 12.0f;
  float userLineCompression = 1.0f;
  bool embeddedStyle = false;
  // Precomputed at section/parser start: index = sizeStep 0..4
  int fontIdByStep[5] = {0, 0, 0, 0, 0};
  bool singleSizeFamily = true;  // SD one-face or unknown ladder → collapse steps
};

// Fill fontIdByStep from known builtin ladders (Bitter / Source Serif 4) or collapse.
void initStyleResolveContext(StyleResolveContext& ctx, int baseFontId, float userLineCompression, bool embeddedStyle,
                             const GfxRenderer& renderer);

[[nodiscard]] inline int resolveRelativeFontId(const StyleResolveContext& ctx, uint8_t step) {
  if (step > SIZE_STEP_MAX) step = SIZE_STEP_MAX;
  const int id = ctx.fontIdByStep[step];
  return id != 0 ? id : ctx.baseFontId;
}

// Casper em unit = font ascender (existing firmware convention).
[[nodiscard]] float casperEmPx(const GfxRenderer& renderer, int fontId);

// Map CSS font-size length (already keyword→em in parser) to a sizeStep relative to baseEmPx.
// Returns SIZE_STEP_BASE when CSS is silent or single-size family collapses scale.
[[nodiscard]] uint8_t sizeStepFromCssFontSize(const CssStyle& css, const StyleResolveContext& ctx);

// Default heading ladder when CSS has no font-size: h1=+2, h2=+1, h3..h6=base.
[[nodiscard]] uint8_t sizeStepForHeadingLevel(int level /*1..6*/, const StyleResolveContext& ctx);

// Resolve block line-height in pixels. line-height % is font-relative (refLinePx), never viewport.
[[nodiscard]] int16_t resolveLineHeightPx(const CssStyle& css, int blockFontId, const StyleResolveContext& ctx,
                                          const GfxRenderer& renderer);

// Apply Rivulet metrics onto a BlockStyle produced by fromCssStyle (sizeStep + lineHeightPx).
// headingLevel: 0 = not a heading; 1..6 = h1..h6.
void applyRivuletBlockMetrics(struct BlockStyle& block, const CssStyle& css, const StyleResolveContext& ctx,
                              const GfxRenderer& renderer, int headingLevel = 0);
