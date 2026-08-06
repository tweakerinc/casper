#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"
#include "Epub/css/StyleResolve.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  // Upper bound (in em) for any single side's horizontal margin or padding.
  // Some EPUBs apply huge em-based insets to chapter-opener classes; without a
  // cap, effectiveWidth collapses to 1-2 words per line and justification dumps
  // the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;
  // Vertical margins often use % of *viewport width* in our CssLength resolver
  // (e.g. .ct1 { margin: 10% 0 8% 25% } → ~50px top on X3). Uncapped, epigraph
  // attribution lines ("— THE STOLEN JOURNALS") get pushed to the next page.
  static constexpr float MAX_VERTICAL_MARGIN_EM = 1.25f;

  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML

  // Rivulet PR1b-min: relative size step (default = user base face) and resolved line advance px.
  // sizeStep 0 means "two steps smaller" — never treat 0 as unset (use SIZE_STEP_BASE).
  uint8_t sizeStep = SIZE_STEP_BASE;
  int16_t lineHeightPx = 0;  // 0 → fall back to renderer.getLineHeight(base) at layout time
  // Drop-cap (and rare absolute faces): non-zero forces TextBlock::render to this fontId
  // instead of resolveRelativeFontId(readerBase, sizeStep). 0 = normal resolve path.
  int paintFontIdOverride = 0;

  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }

    // Direction is not axis-specific. Inherit from parent when child doesn't define it.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }

    // Rivulet metrics: prefer child's size/line-height (result starts as child).
    // If child still has defaults and parent has non-default, inherit parent.
    if (child.sizeStep == SIZE_STEP_BASE && sizeStep != SIZE_STEP_BASE) {
      result.sizeStep = sizeStep;
    }
    if (child.lineHeightPx == 0 && lineHeightPx > 0) {
      result.lineHeightPx = lineHeightPx;
    }

    // fromBrElement is consumed by startNewTextBlock when an empty <br> block
    // is merged with the following paragraph; never propagate it further.
    result.fromBrElement = false;
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    const auto maxVerticalMarginPx = static_cast<int16_t>(emSize * MAX_VERTICAL_MARGIN_EM);
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop =
        std::min(cssStyle.marginTop.toPixelsInt16(emSize, vw), maxVerticalMarginPx);
    blockStyle.marginBottom =
        std::min(cssStyle.marginBottom.toPixelsInt16(emSize, vw), maxVerticalMarginPx);
    blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    blockStyle.paddingTop =
        std::min(cssStyle.paddingTop.toPixelsInt16(emSize, vw), maxVerticalMarginPx);
    blockStyle.paddingBottom =
        std::min(cssStyle.paddingBottom.toPixelsInt16(emSize, vw), maxVerticalMarginPx);
    blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS body copy, unless "Book's Style" is selected.
    // Explicit center/right from the book (chapter titles, epigraphs, pull-quotes)
    // is layout intent and must win over a global Left/Justify preference —
    // otherwise titles look left-biased vs CrossInk when embedded/inline CSS is on.
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else if (blockStyle.textAlignDefined &&
               (cssStyle.textAlign == CssTextAlign::Center || cssStyle.textAlign == CssTextAlign::Right)) {
      blockStyle.alignment = cssStyle.textAlign;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    // RTL direction from CSS/HTML
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = (cssStyle.direction == CssTextDirection::Rtl);
      blockStyle.directionDefined = true;
    }
    return blockStyle;
  }
};
