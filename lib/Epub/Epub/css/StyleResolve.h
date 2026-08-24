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
  // True when ladder could not load multi-size faces (SD one-file or unknown).
  // sizeStep deltas still apply for CSS/headings; paint may use DROP_CAP for larger steps.
  bool singleSizeFamily = true;
};

// Optional app-layer hook: fill fontIdByStep[5] for SD families (multi-size .cpfont packs).
// Return true if a multi-size ladder was filled (sets singleSizeFamily=false).
using StyleLadderFillFn = bool (*)(void* ctx, int baseFontId, int outFontIdByStep[5]);
void setStyleLadderFillHook(StyleLadderFillFn fn, void* ctx);

// Paint path: set real lineCompression / embeddedStyle so ladder cache keys match measure.
void setPaintStyleResolveParams(float userLineCompression, bool embeddedStyle);
void getPaintStyleResolveParams(float& userLineCompression, bool& embeddedStyle);

// Fill fontIdByStep from known builtin ladders or SD hook / collapse.
void initStyleResolveContext(StyleResolveContext& ctx, int baseFontId, float userLineCompression, bool embeddedStyle,
                             const GfxRenderer& renderer);

[[nodiscard]] inline int resolveRelativeFontId(const StyleResolveContext& ctx, uint8_t step) {
  if (step > SIZE_STEP_MAX) step = SIZE_STEP_MAX;
  const int id = ctx.fontIdByStep[step];
  return id != 0 ? id : ctx.baseFontId;
}

// Drop-cap face picker: same-family ladder fonts at body size and larger (not just sizeStep ±2).
// Writes unique fontIds into outIds[0..return-1], small→large. maxOut is the array capacity.
// Falls back to {baseFontId} when the family is single-size / unknown.
[[nodiscard]] int collectDropCapFontCandidates(const StyleResolveContext& ctx, int bodyFontId, int* outIds, int maxOut);

// True when step should use 2× DROP_CAP paint/measure on a single-size face.
[[nodiscard]] inline bool sizeStepNeedsSyntheticScale(const StyleResolveContext& ctx, uint8_t step) {
  return ctx.singleSizeFamily && step > SIZE_STEP_BASE;
}

// CrossPoint em unit = font ascender (existing firmware convention).
[[nodiscard]] float casperEmPx(const GfxRenderer& renderer, int fontId);

// Map CSS font-size length (already keyword→em in parser) to a sizeStep relative to baseEmPx.
// Returns SIZE_STEP_BASE when CSS is silent.
[[nodiscard]] uint8_t sizeStepFromCssFontSize(const CssStyle& css, const StyleResolveContext& ctx);

// Default heading ladder when CSS has no font-size: h1=+2, h2=+1, h3..h6=base.
// Still returns deltas on single-size families so DROP_CAP synthetic scale can apply.
[[nodiscard]] uint8_t sizeStepForHeadingLevel(int level /*1..6*/, const StyleResolveContext& ctx);

// Resolve block line-height in pixels. line-height % is font-relative (refLinePx), never viewport.
[[nodiscard]] int16_t resolveLineHeightPx(const CssStyle& css, int blockFontId, const StyleResolveContext& ctx,
                                          const GfxRenderer& renderer);

// Apply Rivulet metrics onto a BlockStyle produced by fromCssStyle (sizeStep + lineHeightPx).
// headingLevel: 0 = not a heading; 1..6 = h1..h6.
void applyRivuletBlockMetrics(struct BlockStyle& block, const CssStyle& css, const StyleResolveContext& ctx,
                              const GfxRenderer& renderer, int headingLevel = 0);
