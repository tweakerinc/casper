#include "StyleResolve.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "Epub/blocks/BlockStyle.h"

namespace {

// Builtin reader ladders (must match src/fontIds.h). Kept here so lib/Epub does not
// depend on the app layer; bump if font IDs change.
// Order: 10 / 12 / 14 / 16 / 18 pt (matches FONT_SIZE enum after casperReaderFontSizePtMigrated).
constexpr int kLexendDeca[] = {
    -1602494176,  // 10
    -789173636,   // 12
    300363550,    // 14
    -940581834,   // 16
    -2078415541,  // 18
};
constexpr int kSourceSerif[] = {
    1970618696,   // 10
    386902914,    // 12
    -1077864260,  // 14
    1231166843,   // 16
    326065580,    // 18
};
static constexpr int kReaderLadderLen = 5;

int familyIndex(const int* ladder, int fontId) {
  for (int i = 0; i < kReaderLadderLen; ++i) {
    if (ladder[i] == fontId) return i;
  }
  return -1;
}

void fillFromLadder(StyleResolveContext& ctx, const int* ladder, int baseIdx) {
  // Map sizeStep 0..4 → absolute ladder index clamped to [0, kReaderLadderLen-1]
  // delta = step - SIZE_STEP_BASE ∈ [-2,+2]
  const int maxIdx = kReaderLadderLen - 1;
  for (int step = 0; step <= SIZE_STEP_MAX; ++step) {
    const int absIdx = std::clamp(baseIdx + (step - static_cast<int>(SIZE_STEP_BASE)), 0, maxIdx);
    ctx.fontIdByStep[step] = ladder[absIdx];
  }
  ctx.singleSizeFamily = false;
}

void fillCollapsed(StyleResolveContext& ctx) {
  for (int step = 0; step <= SIZE_STEP_MAX; ++step) {
    ctx.fontIdByStep[step] = ctx.baseFontId;
  }
  ctx.singleSizeFamily = true;
}

float clampFactor(float f) { return std::clamp(f, 0.85f, 1.6f); }

}  // namespace

float casperEmPx(const GfxRenderer& renderer, const int fontId) {
  return static_cast<float>(renderer.getFontAscenderSize(fontId));
}

void initStyleResolveContext(StyleResolveContext& ctx, const int baseFontId, const float userLineCompression,
                             const bool embeddedStyle, const GfxRenderer& renderer) {
  ctx.baseFontId = baseFontId;
  ctx.userLineCompression = userLineCompression;
  ctx.embeddedStyle = embeddedStyle;
  ctx.baseEmPx = casperEmPx(renderer, baseFontId);

  const int lexendIdx = familyIndex(kLexendDeca, baseFontId);
  if (lexendIdx >= 0) {
    fillFromLadder(ctx, kLexendDeca, lexendIdx);
    return;
  }
  const int ssIdx = familyIndex(kSourceSerif, baseFontId);
  if (ssIdx >= 0) {
    fillFromLadder(ctx, kSourceSerif, ssIdx);
    return;
  }

  // SD / unknown: no multi-size ladder without registry API in this lib.
  fillCollapsed(ctx);
  // Log once per baseFontId — paint calls initStyleResolveContext every line; spam can flood
  // the serial log and thrash the logger during page turns.
  static int s_loggedCollapsedFontId = 0;
  if (baseFontId != s_loggedCollapsedFontId) {
    s_loggedCollapsedFontId = baseFontId;
    LOG_DBG("RLC", "SD/unknown font ladder collapsed to single size (fontId=%d)", baseFontId);
  }
}

uint8_t sizeStepFromCssFontSize(const CssStyle& css, const StyleResolveContext& ctx) {
  if (!ctx.embeddedStyle || !css.hasFontSize()) {
    return SIZE_STEP_BASE;
  }
  if (ctx.singleSizeFamily) {
    return SIZE_STEP_BASE;
  }

  // Resolve length against base em (Casper convention). Percent is relative to base em, not viewport.
  float px = 0.0f;
  switch (css.fontSize.unit) {
    case CssUnit::Percent:
      px = (css.fontSize.value / 100.0f) * ctx.baseEmPx;
      break;
    case CssUnit::Em:
    case CssUnit::Rem:
    case CssUnit::Points:
    case CssUnit::Pixels:
    default:
      px = css.fontSize.toPixels(ctx.baseEmPx, 0);
      break;
  }
  if (px <= 0.0f || ctx.baseEmPx <= 0.0f) {
    return SIZE_STEP_BASE;
  }

  const float scale = px / ctx.baseEmPx;
  // Discrete ladder bins. Alice .tale1–.tale47 use 99%→53% for the mouse-tail taper:
  // keep smaller steps so the tail visibly shrinks (not only the last third).
  // <0.70 → -2, <0.88 → -1, <1.08 → 0, <1.25 → +1, else +2
  int delta = 0;
  if (scale < 0.70f) {
    delta = -2;
  } else if (scale < 0.88f) {
    delta = -1;
  } else if (scale < 1.08f) {
    delta = 0;
  } else if (scale < 1.25f) {
    delta = 1;
  } else {
    delta = 2;
  }
  return static_cast<uint8_t>(std::clamp(static_cast<int>(SIZE_STEP_BASE) + delta, static_cast<int>(SIZE_STEP_MIN),
                                         static_cast<int>(SIZE_STEP_MAX)));
}

uint8_t sizeStepForHeadingLevel(const int level, const StyleResolveContext& ctx) {
  if (ctx.singleSizeFamily || level < 1) {
    return SIZE_STEP_BASE;
  }
  int delta = 0;
  if (level == 1) {
    delta = 2;
  } else if (level == 2) {
    delta = 1;
  }
  return static_cast<uint8_t>(std::clamp(static_cast<int>(SIZE_STEP_BASE) + delta, static_cast<int>(SIZE_STEP_MIN),
                                         static_cast<int>(SIZE_STEP_MAX)));
}

int16_t resolveLineHeightPx(const CssStyle& css, const int blockFontId, const StyleResolveContext& ctx,
                            const GfxRenderer& renderer) {
  const int refLinePx = renderer.getLineHeight(blockFontId, ctx.userLineCompression);
  if (!ctx.embeddedStyle || !css.hasLineHeight() || css.lineHeightKind == CssLineHeightKind::None) {
    return static_cast<int16_t>(refLinePx);
  }

  int px = refLinePx;
  if (css.lineHeightKind == CssLineHeightKind::Unitless) {
    const float factor = clampFactor(css.lineHeightUnitless);
    px = static_cast<int>(std::lround(factor * static_cast<float>(refLinePx)));
  } else if (css.lineHeightKind == CssLineHeightKind::Length) {
    const float em = casperEmPx(renderer, blockFontId);
    if (css.lineHeightLength.unit == CssUnit::Percent) {
      const float factor = clampFactor(css.lineHeightLength.value / 100.0f);
      px = static_cast<int>(std::lround(factor * static_cast<float>(refLinePx)));
    } else {
      px = static_cast<int>(std::lround(css.lineHeightLength.toPixels(em, 0)));
      const int lo = static_cast<int>(std::lround(0.85f * static_cast<float>(refLinePx)));
      const int hi = static_cast<int>(std::lround(1.6f * static_cast<float>(refLinePx)));
      px = std::clamp(px, lo, hi);
    }
  }
  return static_cast<int16_t>(std::max(1, px));
}

void applyRivuletBlockMetrics(BlockStyle& block, const CssStyle& css, const StyleResolveContext& ctx,
                              const GfxRenderer& renderer, const int headingLevel) {
  uint8_t step = SIZE_STEP_BASE;
  if (ctx.embeddedStyle && css.hasFontSize()) {
    step = sizeStepFromCssFontSize(css, ctx);
  } else if (ctx.embeddedStyle && headingLevel >= 1 && headingLevel <= 6 && !css.hasFontSize()) {
    step = sizeStepForHeadingLevel(headingLevel, ctx);
  }
  block.sizeStep = step;

  const int blockFontId = resolveRelativeFontId(ctx, step);
  block.lineHeightPx = resolveLineHeightPx(css, blockFontId, ctx, renderer);
}
