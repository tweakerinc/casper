#include "StyleResolve.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "Epub/blocks/BlockStyle.h"

namespace {

// Builtin reader ladders (must match src/fontIds.h). Kept here so lib/Epub does not
// depend on the app layer; bump if font IDs change.
// Order: 8 / 10 / 12 / 14 / 16 / 18 pt (matches FONT_SIZE after casperReaderFontSize8Migrated).
constexpr int kLiterata[] = {
    -1128177077,  // 8 → clamp to 10 (no separate 8 body face)
    -1128177077,  // 10
    2090520927,   // 12
    -847079762,   // 14
    -209681255,   // 16
    -209681255,   // 18 → clamp to 16
};
constexpr int kSourceSerif[] = {
    1470095001,   // 8
    -324599973,   // 10
    876380291,    // 12
    426921930,    // 14
    1484141743,   // 16
    652444703,    // 18
};
static constexpr int kReaderLadderLen = 6;

StyleLadderFillFn s_ladderFillFn = nullptr;
void* s_ladderFillCtx = nullptr;

float s_paintLineCompression = 1.0f;
bool s_paintEmbeddedStyle = true;

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

void setStyleLadderFillHook(StyleLadderFillFn fn, void* ctx) {
  s_ladderFillFn = fn;
  s_ladderFillCtx = ctx;
}

void setPaintStyleResolveParams(const float userLineCompression, const bool embeddedStyle) {
  s_paintLineCompression = userLineCompression;
  s_paintEmbeddedStyle = embeddedStyle;
}

void getPaintStyleResolveParams(float& userLineCompression, bool& embeddedStyle) {
  userLineCompression = s_paintLineCompression;
  embeddedStyle = s_paintEmbeddedStyle;
}

float casperEmPx(const GfxRenderer& renderer, const int fontId) {
  return static_cast<float>(renderer.getFontAscenderSize(fontId));
}

void initStyleResolveContext(StyleResolveContext& ctx, const int baseFontId, const float userLineCompression,
                             const bool embeddedStyle, const GfxRenderer& renderer) {
  ctx.baseFontId = baseFontId;
  ctx.userLineCompression = userLineCompression;
  ctx.embeddedStyle = embeddedStyle;
  ctx.baseEmPx = casperEmPx(renderer, baseFontId);

  const int literataIdx = familyIndex(kLiterata, baseFontId);
  if (literataIdx >= 0) {
    fillFromLadder(ctx, kLiterata, literataIdx);
    return;
  }
  const int ssIdx = familyIndex(kSourceSerif, baseFontId);
  if (ssIdx >= 0) {
    fillFromLadder(ctx, kSourceSerif, ssIdx);
    return;
  }

  // SD / unknown: try app hook to load multi-size .cpfont ladder.
  if (s_ladderFillFn && s_ladderFillFn(s_ladderFillCtx, baseFontId, ctx.fontIdByStep)) {
    ctx.singleSizeFamily = false;
    // Ensure base step maps to the caller's baseFontId when possible.
    if (ctx.fontIdByStep[SIZE_STEP_BASE] == 0) {
      ctx.fontIdByStep[SIZE_STEP_BASE] = baseFontId;
    }
    return;
  }

  fillCollapsed(ctx);
  // Log once per baseFontId — paint re-inits when base changes.
  static int s_loggedCollapsedFontId = 0;
  if (baseFontId != s_loggedCollapsedFontId) {
    s_loggedCollapsedFontId = baseFontId;
    LOG_INF("RLC", "SD/unknown font ladder collapsed (fontId=%d); headings use synthetic scale", baseFontId);
  }
}

int collectDropCapFontCandidates(const StyleResolveContext& ctx, const int bodyFontId, int* outIds,
                                 const int maxOut) {
  if (!outIds || maxOut <= 0) return 0;

  auto pushUnique = [&](const int id, int& n) {
    if (id == 0 || n >= maxOut) return;
    for (int i = 0; i < n; ++i) {
      if (outIds[i] == id) return;
    }
    outIds[n++] = id;
  };

  int n = 0;

  // Full builtin 6-rung ladder (8–18): drop caps need faces beyond sizeStep ±2
  // (body 10pt → step max is only 14pt; 16/18 are required to fill a 2-line zone).
  for (const int* ladder : {kLiterata, kSourceSerif}) {
    int idx = familyIndex(ladder, bodyFontId);
    if (idx < 0) {
      idx = familyIndex(ladder, ctx.baseFontId);
    }
    if (idx >= 0) {
      for (int i = idx; i < kReaderLadderLen; ++i) {
        pushUnique(ladder[i], n);
      }
      pushUnique(bodyFontId, n);
      return n;
    }
  }

  // SD multi-size / unknown: body first, then every loaded step face.
  pushUnique(bodyFontId, n);
  for (int step = 0; step <= SIZE_STEP_MAX; ++step) {
    pushUnique(ctx.fontIdByStep[step], n);
  }
  pushUnique(ctx.baseFontId, n);
  return n;
}

uint8_t sizeStepFromCssFontSize(const CssStyle& css, const StyleResolveContext& ctx) {
  if (!ctx.embeddedStyle || !css.hasFontSize()) {
    return SIZE_STEP_BASE;
  }
  // Keep relative steps even on single-size faces so paint can DROP_CAP-scale larger steps.

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

uint8_t sizeStepForHeadingLevel(const int level, const StyleResolveContext& /*ctx*/) {
  if (level < 1) {
    return SIZE_STEP_BASE;
  }
  // Always return heading deltas — even when singleSizeFamily (synthetic DROP_CAP on paint).
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
  // Small-caps: slightly smaller face + uppercase paint (see TextBlock::render).
  if (ctx.embeddedStyle && css.hasFontVariant() && css.fontVariant == CssFontVariant::SmallCaps) {
    block.smallCaps = true;
    if (step > SIZE_STEP_MIN) {
      step = static_cast<uint8_t>(step - 1);
    }
  } else {
    block.smallCaps = false;
  }
  block.sizeStep = step;

  const int blockFontId = resolveRelativeFontId(ctx, step);
  block.lineHeightPx = resolveLineHeightPx(css, blockFontId, ctx, renderer);

  // Single-size family: mark for synthetic scale so layout can reserve 2× height for large steps.
  block.syntheticScale = sizeStepNeedsSyntheticScale(ctx, step);
}
