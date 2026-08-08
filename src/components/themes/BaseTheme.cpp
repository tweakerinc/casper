#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconGap = 4;
constexpr int bookmarkStatusIconTopCrop = 2;

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const uint8_t displayMode) const {
  // Left aligned: icon on left, percentage on right (or percent-only text at rect.x).
  using M = CrossPointSettings::BATTERY_DISPLAY_MODE;
  const uint8_t mode = displayMode < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                           ? displayMode
                           : static_cast<uint8_t>(M::BATTERY_DISPLAY_ICON_PERCENT);
  const bool showIcon = mode != M::BATTERY_DISPLAY_PERCENT;
  const bool showPct = mode != M::BATTERY_DISPLAY_ICON;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;
  // Reader chrome uses Manage Reader UI → Font Size; system bar callers stay on 8 pt
  // unless they pass a different path (drawStatusBar uses getStatusBarFontId via override below).
  const int pctFont = SETTINGS.getStatusBarFontId();

  if (showIcon) {
    const Rect iconRect{rect.x, y, rect.width, rect.height};
    drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
    fillBatteryIcon(renderer, iconRect, percentage);
  }
  if (showPct) {
    const auto percentageText = std::to_string(percentage) + "%";
    if (showIcon) {
      renderer.drawText(pctFont, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
    } else {
      renderer.drawText(pctFont, rect.x, rect.y, percentageText.c_str());
    }
  }
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const uint8_t displayMode) const {
  // Right aligned: percentage on left of icon, icon flush right (or percent-only flush right).
  // rect is the icon slot; its right edge (rect.x + rect.width) is the flush-right anchor.
  using M = CrossPointSettings::BATTERY_DISPLAY_MODE;
  const uint8_t mode = displayMode < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                           ? displayMode
                           : static_cast<uint8_t>(M::BATTERY_DISPLAY_ICON_PERCENT);
  const bool showIcon = mode != M::BATTERY_DISPLAY_PERCENT;
  const bool showPct = mode != M::BATTERY_DISPLAY_ICON;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;
  const int rightEdge = rect.x + rect.width;
  const int pctFont = SETTINGS.getStatusBarFontId();

  if (showIcon) {
    const Rect iconRect{rect.x, y, rect.width, rect.height};
    drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
    fillBatteryIcon(renderer, iconRect, percentage);
  }
  if (showPct) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(pctFont, percentageText.c_str());
    if (showIcon) {
      renderer.drawText(pctFont, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
    } else {
      renderer.drawText(pctFont, rightEdge - textWidth, rect.y, percentageText.c_str());
    }
  }
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  // Canonical front-button chrome — same geometry on home, settings, dictionary,
  // and every theme. buttonHintsHeight is both reserved layout height and drawn
  // button height (flush to the physical front-button edge).
  if (gpio.hasTouch()) {
    return;
  }

  // Portrait / Portrait inverted: draw in the *live* orientation so labels stay
  // readable with the page (Portrait 180 was forcing Portrait chrome → upside-down
  // words when the device is held inverted). Landscape still forces Portrait so
  // the strip stays on the physical button edge without a full landscape layout.
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool landscape =
      orig_orientation == GfxRenderer::LandscapeClockwise || orig_orientation == GfxRenderer::LandscapeCounterClockwise;
  const bool inverted = orig_orientation == GfxRenderer::PortraitInverted;
  if (landscape) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int cornerRadius = 6;
  constexpr int textYOffset = 7;  // top of button → text baseline (SMALL_FONT)
  const int buttonHeight = metrics.buttonHintsHeight;
  // Portrait: bar at logical bottom (physical front keys). Inverted: logical top
  // maps to the same physical edge when the page is rotated 180°.
  const int barY = inverted ? 0 : (pageHeight - buttonHeight);
  const int stubY = inverted ? 0 : (pageHeight - smallButtonHeight);
  // X3 is wider in portrait (528 vs 480).
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Mirror X in inverted so slot i stays over the same physical key.
    int x = buttonPositions[i];
    if (inverted) {
      x = pageWidth - x - buttonWidth;
    }
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Open the edge toward page content (bottom open in portrait; top open when inverted).
      const bool roundTop = !inverted;
      const bool roundBottom = inverted;
      renderer.fillRoundedRect(x, barY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, barY, buttonWidth, buttonHeight, 1, cornerRadius, roundTop, roundTop, roundBottom,
                               roundBottom, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, barY + textYOffset, labels[i]);
    } else {
      // Empty slot: short stub so the bar rhythm stays consistent across screens.
      renderer.fillRoundedRect(x, stubY, buttonWidth, smallButtonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, stubY, buttonWidth, smallButtonHeight, 1, cornerRadius, !inverted, !inverted,
                               inverted, inverted, true);
    }
  }

  if (landscape) {
    renderer.setOrientation(orig_orientation);
  }
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

namespace {
// Row height for shared lists (Library / Recents / Settings). Short titles stay
// single-line dense; wrapped titles use tighter line step and a taller row only
// for that item.
constexpr int kBaseTitleSubtitleGap = 2;
constexpr int kBaseRowPad = 8;

int baseTitleLineStep(const GfxRenderer& renderer, const int titleFont, const int nLines) {
  const int advanceY = renderer.getLineHeight(titleFont);
  if (nLines <= 1) return advanceY;
  return std::max(18, (advanceY * 7) / 10);
}

int baseTitleBlockHeight(const GfxRenderer& renderer, const int titleFont, const int nLines) {
  const int advanceY = renderer.getLineHeight(titleFont);
  const int step = baseTitleLineStep(renderer, titleFont, nLines);
  return (nLines <= 1) ? advanceY : ((nLines - 1) * step + advanceY);
}

int computeListRowHeightForLines(const GfxRenderer& renderer, const bool hasSubtitle, const int nTitleLines) {
  const int titleFont = SETTINGS.getMenuListFontId();
  int contentH = baseTitleBlockHeight(renderer, titleFont, std::max(1, nTitleLines));
  if (hasSubtitle) {
    contentH += kBaseTitleSubtitleGap + renderer.getLineHeight(SMALL_FONT_ID);
  }
  const int computed = contentH + kBaseRowPad;
  const int baseline = hasSubtitle ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  return std::max(baseline, computed);
}

int computeListRowHeight(const GfxRenderer& renderer, const bool hasSubtitle) {
  return computeListRowHeightForLines(renderer, hasSubtitle, 1);
}
}  // namespace

int BaseTheme::getListRowStep(bool hasSubtitle) const {
  // Approximate step without GfxRenderer (page-size math). drawList measures with
  // the live font for exact row height when painting.
  // Split-title mode does not inflate step — wrap is per-item when text needs it.
  int rowHeight = hasSubtitle ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  if (SETTINGS.menuFontSize == CrossPointSettings::MENU_FONT_LARGE) {
    rowHeight += 4;
  } else if (SETTINGS.menuFontSize == CrossPointSettings::MENU_FONT_XSMALL) {
    rowHeight = std::max(26, rowHeight - 4);
  } else if (SETTINGS.menuFontSize == CrossPointSettings::MENU_FONT_SMALL) {
    rowHeight = std::max(28, rowHeight - 2);
  }
  return rowHeight;
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const int rowStep = getListRowStep(hasSubtitle);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& rowApplied) const {
  (void)highlightValue;
  // Icons reserved title width for no gain on Bare/Penumbra; ignore rowIcon.
  (void)rowIcon;

  const bool hasSubtitleCb = (rowSubtitle != nullptr);
  const int singleRowH = computeListRowHeight(renderer, hasSubtitleCb);
  int pageItems = singleRowH > 0 ? std::max(1, rect.height / singleRowH) : 1;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  int contentWidth = rect.width - 5;
  constexpr int minValueGap = 10;
  constexpr int kRadioR = 5;
  constexpr int kRadioReserve = kRadioR * 2 + 8;

  const int titleFont = SETTINGS.getMenuListFontId();
  const int titleMaxLines = SETTINGS.getMenuListTitleMaxLines();

  // Cumulative Y: wrapped titles get a taller row; short names stay dense.
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int itemY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const bool focused = (i == selectedIndex);
    const bool applied = rowApplied && rowApplied(i);

    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;
    if (applied) rowTextWidth -= kRadioReserve;

    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        int maxValW = std::max(0, rowTextWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(titleFont, valueText.c_str(), maxValW);
        int valueWidth = renderer.getTextWidth(titleFont, valueText.c_str()) + minValueGap;
        rowTextWidth -= valueWidth;
      }
    }

    const auto itemName = rowTitle(i);
    std::vector<std::string> titleLines =
        renderer.wrappedText(titleFont, itemName.c_str(), rowTextWidth, titleMaxLines, EpdFontFamily::REGULAR);
    if (titleLines.empty()) titleLines.emplace_back("");
    // Ellipsize last line if the full title still does not fit.
    if (static_cast<int>(titleLines.size()) >= titleMaxLines) {
      titleLines.back() = renderer.truncatedText(titleFont, titleLines.back().c_str(), rowTextWidth);
    }
    const int nTitleLines = static_cast<int>(titleLines.size());
    const int lineStep = baseTitleLineStep(renderer, titleFont, nTitleLines);
    const int titleBlockH = baseTitleBlockHeight(renderer, titleFont, nTitleLines);

    std::string subtitleDrawn;
    int subtitleLineH = 0;
    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        subtitleDrawn = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        subtitleLineH = renderer.getLineHeight(SMALL_FONT_ID);
      }
    }

    const int rowHeight = computeListRowHeightForLines(renderer, !subtitleDrawn.empty() || hasSubtitleCb, nTitleLines);
    if (i > pageStartIndex && itemY + rowHeight > rect.y + rect.height) {
      break;
    }

    // Focus: bold only — no outline/fill (boxes still ghosted on FAST scroll).
    const int blockH = titleBlockH + (subtitleDrawn.empty() ? 0 : (kBaseTitleSubtitleGap + subtitleLineH));
    int textY = itemY + std::max(0, (rowHeight - blockH) / 2);
    const int textX = rect.x + BaseMetrics::values.contentSidePadding;
    const auto focusStyle = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    for (size_t li = 0; li < titleLines.size(); ++li) {
      const int ly = textY + static_cast<int>(li) * lineStep;
      renderer.drawText(titleFont, textX, ly, titleLines[li].c_str(), /*black=*/true, focusStyle);
    }

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && !focused) {
      const int dimH = renderer.getLineHeight(titleFont);
      for (size_t li = 0; li < titleLines.size(); ++li) {
        const int ly = textY + static_cast<int>(li) * lineStep;
        const int titleWidth = renderer.getTextWidth(titleFont, titleLines[li].c_str());
        for (int py = ly; py < ly + dimH; py++)
          for (int px = textX; px < textX + titleWidth; px++)
            if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
      }
    }

    if (!subtitleDrawn.empty()) {
      const int subtitleY = textY + titleBlockH + kBaseTitleSubtitleGap;
      renderer.drawText(SMALL_FONT_ID, textX, subtitleY, subtitleDrawn.c_str(), /*black=*/true);
    }

    if (!valueText.empty()) {
      const auto valueTextWidth = renderer.getTextWidth(titleFont, valueText.c_str(), focusStyle);
      renderer.drawText(titleFont, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        textY, valueText.c_str(), /*black=*/true, focusStyle);
    }

    // Applied/current setting: filled radio circle (stays put while focus moves).
    if (applied) {
      const int cx = rect.x + contentWidth - BaseMetrics::values.contentSidePadding - kRadioR;
      const int cy = itemY + rowHeight / 2 - 1;
      const bool ink = true;  // black on white (no invert focus bar)
      for (int dy = -kRadioR; dy <= kRadioR; ++dy) {
        for (int dx = -kRadioR; dx <= kRadioR; ++dx) {
          if (dx * dx + dy * dy <= kRadioR * kRadioR) {
            renderer.drawPixel(cx + dx, cy + dy, ink);
          }
        }
      }
    }

    itemY += rowHeight;
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // System top chrome: Left / Middle / Right (Display → Status Bar).
  drawSystemStatusBar(renderer, rect.y, nullptr);

  // Full-width rule under the title band (shared edge with the tab bar below).
  // 1px: 3px rules left heavy residual on FAST settings navigation.
  constexpr int kHeaderRuleThickness = 1;
  const int ruleY = rect.y + rect.height - kHeaderRuleThickness;

  const int sideReserve = systemStatusSideReserve(renderer);
  const int maxTitleWidth = std::max(40, rect.width - sideReserve * 2);

  if (title && title[0] != '\0') {
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    // Center title between real status chrome (clock/battery) and the bottom rule.
    const int chromeBottom = rect.y + kTopChromeBatteryY + 6 + BaseMetrics::values.batteryHeight;
    const int titleY = chromeBottom + std::max(0, (ruleY - chromeBottom - lineH) / 2);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  if (title && title[0] != '\0') {
    renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kHeaderRuleThickness, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    // Keep version away from the centered title / bottom rule.
    const int subY = rect.y + std::max(2, BaseMetrics::values.batteryBarHeight / 2 - 4);
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  // Match drawHeader / Preview rule thickness (thin for e-ink residual).
  constexpr int kRuleThickness = 1;
  // Same air as Manage Fonts Preview label band (lineH + 10).
  constexpr int kBandExtraPad = 10;

  if (tabs.empty()) {
    const int ruleY = rect.y + rect.height - kRuleThickness;
    renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kRuleThickness, true);
    return;
  }

  // Equal-width slots; largest font where every label (incl. "Download") fits.
  const int slotW = std::max(1, rect.width / static_cast<int>(tabs.size()));
  const int maxTextW = std::max(8, slotW - 6);
  static constexpr int kFontCandidates[] = {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};
  int fontId = SMALL_FONT_ID;
  for (const int candidate : kFontCandidates) {
    bool fits = true;
    for (const auto& tab : tabs) {
      // Bold is wider — size against the selected style so chips never clip.
      if (renderer.getTextWidth(candidate, tab.label, EpdFontFamily::BOLD) > maxTextW) {
        fits = false;
        break;
      }
    }
    if (fits) {
      fontId = candidate;
      break;
    }
  }

  const int lineHeight = renderer.getLineHeight(fontId);
  // Header bottom rule is the top edge of this rect; bottom rule sits at rect bottom.
  // Center labels in a Preview-sized band so they do not crowd either rule.
  const int availH = std::max(1, rect.height - kRuleThickness);
  const int contentBandH = lineHeight + kBandExtraPad;
  const int contentTop = rect.y + std::max(0, (availH - contentBandH) / 2);
  const int textY = contentTop + (contentBandH - lineHeight) / 2;

  for (size_t i = 0; i < tabs.size(); i++) {
    const auto& tab = tabs[i];
    // Active tab: bold only (no underline / black fill).
    const auto style = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(fontId, tab.label, style);
    const int slotX = rect.x + static_cast<int>(i) * slotW;
    const int textX = slotX + (slotW - textWidth) / 2;
    renderer.drawText(fontId, textX, textY, tab.label, /*black=*/true, style);
  }

  const int ruleY = rect.y + rect.height - kRuleThickness;
  renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kRuleThickness, true);
  (void)selected;
}

bool BaseTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  (void)renderer;
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height || x < rect.x || x >= rect.x + rect.width) {
    return false;
  }

  const int slotW = std::max(1, rect.width / static_cast<int>(tabs.size()));
  index = std::min(static_cast<int>(tabs.size()) - 1, (x - rect.x) / slotW);
  return true;
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                    const BookReadingStats* stats, float progressPercent,
                                    const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)stats;
  (void)progressPercent;
  (void)globalStats;
  (void)currentChapterTitle;
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Snapshot the cover card only (not the whole home tile / framebuffer).
          coverBufferStored = storeCoverBuffer(bookX, bookY, bookWidth, bookHeight);
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;  // Home menu is text-only; icons unused.
  for (int i = 0; i < buttonCount; ++i) {
    // Layout from rect.y (caller vertically centers the full stack).
    const int tileY =
        rect.y + static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;
    // Bold only for selection; labels centered (no icons).
    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const auto style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, style);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, /*black=*/true, style);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message, float topOffsetRatio, bool refresh) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  int y;
  // kPopupCenterY (-2) or any value < -1.5 means vertical center.
  if (topOffsetRatio < -1.5f) {
    y = (renderer.getScreenHeight() - h) / 2;
  } else if (topOffsetRatio >= 0.0f) {
    y = static_cast<int>(renderer.getScreenHeight() * topOffsetRatio);
  } else {
    y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  }

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, metrics.popupTextInverted, popupFontFamily);
  if (refresh) {
    renderer.displayBuffer();
  }
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress,
                                  bool refresh) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    if (refresh) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  // Prefer HALF over FAST: FAST during cover gen caused half-panel / ghosted frames.
  if (refresh) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string bookTitle, const int paddingBottom,
                              const int textYOffset, const bool fillMargin, const bool isPageBookmarked,
                              const bool pageCountEstimated, const char* timeLeftBookLabel,
                              const char* timeLeftChapterLabel, const bool drawTopBattery, const int bookPage,
                              const int bookPageCount, const bool bookPageCountEstimated, const int chapterIndex,
                              const int chapterTotal, std::string chapterTitle,
                              const bool previewIgnoreBatteryMasterHide, const char* previewClockTime) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  // Master battery visibility (Settings → Display → Battery Show/Hide). Preview
  // can ignore the master so Customize Reader UI still shows Battery slots.
  const bool batteryMasterOn =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  const bool batteryAllowed = batteryMasterOn || previewIgnoreBatteryMasterHide;
  // Preview can ignore master hide but still uses the reader's battery display mode.
  const uint8_t batteryDisplay = sb.batteryDisplay < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                                     ? sb.batteryDisplay
                                     : static_cast<uint8_t>(CrossPointSettings::BATTERY_DISPLAY_ICON_PERCENT);
  const bool showBattIcon = batteryAllowed && batteryDisplay != CrossPointSettings::BATTERY_DISPLAY_PERCENT;
  const bool showBattPct = batteryAllowed && batteryDisplay != CrossPointSettings::BATTERY_DISPLAY_ICON;

  const int topTextY = metrics.topPadding + kTopChromeBatteryY;
  const int leftX = orientedMarginLeft + kTopChromeInsetX;
  const int rightX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  const int screenW = renderer.getScreenWidth();
  const int centerX = screenW / 2;

  const int bookCur = (bookPage > 0 && bookPageCount > 0) ? bookPage : currentPage;
  const int bookTot = (bookPage > 0 && bookPageCount > 0) ? bookPageCount : pageCount;
  const bool bookEst = (bookPage > 0 && bookPageCount > 0) ? bookPageCountEstimated : pageCountEstimated;

  // Both page counters use the same "Pg." form; scope is chosen in settings.
  auto chapterPageText = [&](char* buf, size_t len) {
    if (pageCountEstimated) {
      snprintf(buf, len, "~ Pg. %d/%d", currentPage, pageCount);
    } else {
      snprintf(buf, len, "Pg. %d/%d", currentPage, pageCount);
    }
  };
  auto bookPageText = [&](char* buf, size_t len) {
    if (bookEst) {
      snprintf(buf, len, "~ Pg. %d/%d", bookCur, bookTot);
    } else {
      snprintf(buf, len, "Pg. %d/%d", bookCur, bookTot);
    }
  };
  // TOC chapter position (not pages): "Ch. 5/40".
  auto chapterIndexText = [&](char* buf, size_t len) -> const char* {
    if (chapterIndex <= 0 || chapterTotal <= 0) return nullptr;
    snprintf(buf, len, "Ch. %d/%d", chapterIndex, chapterTotal);
    return buf;
  };
  auto progressPercentText = [&](char* buf, size_t len) {
    snprintf(buf, len, "%.0f%% %s", bookProgress, tr(STR_COMPLETE));
  };
  auto clockText = [&](char* buf, size_t len) -> const char* {
    // Settings preview: always show a sample clock so layout can be judged without RTC.
    if (previewClockTime != nullptr && previewClockTime[0] != '\0') {
      return previewClockTime;
    }
    if (!halClock.isAvailable()) return nullptr;
    if (!halClock.formatTime(buf, len, SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) return nullptr;
    return buf;
  };

  const int chromeFont = SETTINGS.getStatusBarFontId();

  // align: 0=left, 1=center, 2=right. Returns drawn width (0 if empty).
  auto drawSlot = [&](uint8_t content, int anchorX, int y, int align, int maxWidth) -> int {
    using C = CrossPointSettings::STATUS_BAR_CORNER_CONTENT;
    if (content == C::CORNER_HIDE) return 0;

    if (content == C::CORNER_BATTERY) {
      if (!batteryAllowed) return 0;
      if (!drawTopBattery && y == topTextY) return 0;
      const int iconW = metrics.batteryWidth;
      const int pctW = showBattPct ? renderer.getTextWidth(chromeFont, "100%") : 0;
      int groupW = 0;
      if (showBattIcon && showBattPct) {
        groupW = iconW + batteryPercentSpacing + pctW;
      } else if (showBattIcon) {
        groupW = iconW;
      } else {
        groupW = pctW;
      }
      int iconX = anchorX;
      if (align == 2) {
        // Right: group flush right; icon at right edge when shown.
        iconX = showBattIcon ? (anchorX - iconW) : (anchorX - groupW);
        drawBatteryRight(renderer, Rect{showBattIcon ? iconX : (anchorX - iconW), y, iconW, metrics.batteryHeight},
                         batteryDisplay);
      } else if (align == 1) {
        iconX = anchorX - groupW / 2;
        drawBatteryLeft(renderer, Rect{iconX, y, iconW, metrics.batteryHeight}, batteryDisplay);
      } else {
        drawBatteryLeft(renderer, Rect{anchorX, y, iconW, metrics.batteryHeight}, batteryDisplay);
      }
      return groupW;
    }

    char buf[48];
    const char* text = nullptr;
    std::string titleScratch;
    switch (content) {
      case C::CORNER_CHAPTER_PAGE_COUNTER:
        chapterPageText(buf, sizeof(buf));
        text = buf;
        break;
      case C::CORNER_BOOK_PAGE_COUNTER:
        bookPageText(buf, sizeof(buf));
        text = buf;
        break;
      case C::CORNER_CHAPTER_COUNTER:
        text = chapterIndexText(buf, sizeof(buf));
        break;
      case C::CORNER_PROGRESS_PERCENT:
        progressPercentText(buf, sizeof(buf));
        text = buf;
        break;
      case C::CORNER_TIME_LEFT_BOOK:
        text = (timeLeftBookLabel && timeLeftBookLabel[0]) ? timeLeftBookLabel : nullptr;
        break;
      case C::CORNER_TIME_LEFT_CHAPTER:
        text = (timeLeftChapterLabel && timeLeftChapterLabel[0]) ? timeLeftChapterLabel : nullptr;
        break;
      case C::CORNER_CLOCK:
        text = clockText(buf, sizeof(buf));
        break;
      case C::CORNER_BOOK_TITLE:
        if (bookTitle.empty()) return 0;
        titleScratch = bookTitle;
        if (maxWidth > 0) {
          titleScratch = renderer.truncatedText(chromeFont, titleScratch.c_str(), maxWidth);
        }
        text = titleScratch.c_str();
        break;
      case C::CORNER_CHAPTER_TITLE:
        if (chapterTitle.empty()) return 0;
        titleScratch = chapterTitle;
        if (maxWidth > 0) {
          titleScratch = renderer.truncatedText(chromeFont, titleScratch.c_str(), maxWidth);
        }
        text = titleScratch.c_str();
        break;
      case C::CORNER_XTC_STATUS_BAR:
        // Placement-only marker (drives XTC top/bottom overlay). No reader chrome text.
        return 0;
      default:
        return 0;
    }
    if (!text || !text[0]) return 0;
    int w = renderer.getTextWidth(chromeFont, text);
    if (maxWidth > 0 && w > maxWidth && content != C::CORNER_BOOK_TITLE && content != C::CORNER_CHAPTER_TITLE) {
      // Non-title strings are short; skip rather than overflow corners.
      return 0;
    }
    int x = anchorX;
    if (align == 2)
      x = anchorX - w;
    else if (align == 1)
      x = anchorX - w / 2;
    renderer.drawText(chromeFont, x, y, text);
    return w;
  };

  // ---- Top row: left / middle / right ----
  const int topSideBudget = std::max(40, (screenW / 2) - 24);
  drawSlot(sb.upperLeft, leftX, topTextY, 0, topSideBudget);
  drawSlot(sb.upperRight, rightX, topTextY, 2, topSideBudget);
  drawSlot(sb.upperMiddle, centerX, topTextY, 1, std::max(40, screenW - 160));

  // ---- Bottom lane + progress bar ----
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = std::max(0, renderer.getScreenWidth() - barMarginLeft - barMarginRight);
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    // bookProgress is 0–100 float; never cast through size_t (truncates 0–1 by mistake to 0).
    float progressPct = 0.0f;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progressPct = bookProgress;
    } else if (pageCount > 0) {
      progressPct = (static_cast<float>(currentPage) / static_cast<float>(pageCount)) * 100.0f;
    }
    if (progressPct < 0.0f) progressPct = 0.0f;
    if (progressPct > 100.0f) progressPct = 100.0f;
    const int barWidth =
        std::max(0, static_cast<int>((static_cast<float>(progressBarMaxWidth) * progressPct) / 100.0f + 0.5f));
    const int barHeight = std::max(static_cast<int>(sb.progressBarHeightPx),
                                   sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0));
    if (barWidth > 0 && barHeight > 0) {
      renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
    }
  }

  if (isPageBookmarked) {
    drawBookmarkStatusIcon(renderer, leftClusterX, textY + 5);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkStatusIconGap;
  }

  // Lower left (after bookmark).
  {
    const int x0 = leftClusterX + leftClusterWidth;
    const int w = drawSlot(sb.lowerLeft, x0, textY, 0, 0);
    if (w > 0) leftClusterWidth += w + 8;
  }
  // Lower right.
  {
    const int w = drawSlot(sb.lowerRight, rightClusterX, textY, 2, 0);
    if (w > 0) rightClusterWidth += w;
  }

  // Lower middle — centered in remaining lane (title-aware truncation).
  if (sb.lowerMiddle != CrossPointSettings::CORNER_HIDE) {
    const int midY = textY - textYOffset;
    const int usable = screenW - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;
    const int sidePad = std::max(leftClusterWidth, rightClusterWidth) + 24;
    const int midMax = std::max(40, usable - 2 * sidePad);
    drawSlot(sb.lowerMiddle, centerX, midY, 1, midMax);
  }
}

void BaseTheme::drawTopStatusBarClock(const GfxRenderer& renderer, int topY, const char* previewTime) const {
  // Visibility is decided by the caller (system header vs reader status bar).
  // Always centers — used by reader CORNER_CLOCK middle and legacy call sites.
  char timeBuf[9];
  const char* timeText = previewTime;
  if (timeText == nullptr) {
    if (!halClock.isAvailable()) {
      return;
    }
    const bool clock12h = SETTINGS.clockFormat == 1;
    if (!halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, clock12h)) {
      return;
    }
    timeText = timeBuf;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  (void)orientedMarginRight;
  (void)orientedMarginBottom;
  (void)orientedMarginLeft;

  // Same SMALL_FONT_ID as battery % for a uniform top row.
  constexpr int kClockFont = SMALL_FONT_ID;
  const int textWidth = renderer.getTextWidth(kClockFont, timeText);
  const int lineHeight = renderer.getLineHeight(kClockFont);
  const int textX = (renderer.getScreenWidth() - textWidth) / 2;
  const int baseTopY = topY >= 0 ? topY : orientedMarginTop + metrics.topPadding;
  const int textY = baseTopY + std::max(2, (metrics.statusBarVerticalMargin - lineHeight) / 2);
  renderer.drawText(kClockFont, textX, textY, timeText);
}

int BaseTheme::systemStatusSideReserve(const GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int reserve = kTopChromeInsetX + metrics.contentSidePadding;
  // Battery with percent is the widest typical side content.
  const int battW = metrics.batteryWidth + batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, "100%");
  // Clock sample for 12h (wider) — "12:00 PM".
  const int clockW = renderer.getTextWidth(SMALL_FONT_ID, "12:00 PM");
  const int sideContent = std::max(battW, clockW);
  // If either outer slot has content, reserve for title centering.
  if (SETTINGS.systemStatusBarLeft != CrossPointSettings::SYS_SLOT_HIDE ||
      SETTINGS.systemStatusBarRight != CrossPointSettings::SYS_SLOT_HIDE) {
    reserve += sideContent;
  } else if (SETTINGS.systemStatusBarMiddle != CrossPointSettings::SYS_SLOT_HIDE) {
    // Middle-only: still leave a modest margin so title does not collide with center chrome.
    reserve += sideContent / 2;
  }
  return reserve;
}

void BaseTheme::drawSystemStatusBar(const GfxRenderer& renderer, int topY, const char* previewTime,
                                    const bool forceBatteryWarningPreview) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  (void)orientedMarginBottom;

  const int baseTopY = topY >= 0 ? topY : orientedMarginTop + metrics.topPadding;
  const int batteryY = baseTopY + kTopChromeBatteryY;
  const int screenW = renderer.getScreenWidth();
  const int leftX = orientedMarginLeft + kTopChromeInsetX;
  const int rightX = screenW - orientedMarginRight - kTopChromeInsetX;
  const int centerX = screenW / 2;

  // Clear full chrome band so placement changes / digit count do not ghost.
  {
    const int clearH = std::max(metrics.batteryHeight + 10, metrics.statusBarVerticalMargin);
    renderer.fillRect(orientedMarginLeft, baseTopY, screenW - orientedMarginLeft - orientedMarginRight, clearH, false);
  }

  char timeBuf[16];
  const char* timeText = previewTime;
  if (timeText == nullptr && SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK)) {
    if (halClock.isAvailable()) {
      if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
        timeText = timeBuf;
      }
    }
  }

  // Battery Warning text when SoC ≤ threshold (or forced in Status Bar preview).
  // Drawn in the slot that holds SYS_SLOT_BATTERY_WARNING (default Middle).
  const int warnThr = SETTINGS.batteryWarningThresholdPercent();
  const uint16_t battPctLive = powerManager.getBatteryPercentage();
  const bool showBatteryWarning =
      forceBatteryWarningPreview || (warnThr > 0 && static_cast<int>(battPctLive) <= warnThr);
  const int warnPctShow = forceBatteryWarningPreview ? (warnThr > 0 ? warnThr : 15) : static_cast<int>(battPctLive);

  auto drawClockAt = [&](int align) {
    if (timeText == nullptr || timeText[0] == '\0') return;
    constexpr int kClockFont = SMALL_FONT_ID;
    const int textWidth = renderer.getTextWidth(kClockFont, timeText);
    const int lineHeight = renderer.getLineHeight(kClockFont);
    const int textY = baseTopY + std::max(2, (metrics.statusBarVerticalMargin - lineHeight) / 2);
    int textX = centerX - textWidth / 2;
    if (align == 0)
      textX = leftX;
    else if (align == 2)
      textX = rightX - textWidth;
    renderer.drawText(kClockFont, textX, textY, timeText);
  };

  auto drawBatteryAt = [&](int align) {
    const int battW = metrics.batteryWidth;
    const int battH = metrics.batteryHeight;
    const uint8_t mode = SETTINGS.systemBatteryDisplay < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                             ? SETTINGS.systemBatteryDisplay
                             : static_cast<uint8_t>(CrossPointSettings::BATTERY_DISPLAY_ICON_PERCENT);
    const bool showIcon = mode != CrossPointSettings::BATTERY_DISPLAY_PERCENT;
    const bool showPct = mode != CrossPointSettings::BATTERY_DISPLAY_ICON;
    const int pctW = showPct ? renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0;
    int groupW = 0;
    if (showIcon && showPct) {
      groupW = battW + batteryPercentSpacing + pctW;
    } else if (showIcon) {
      groupW = battW;
    } else {
      groupW = pctW;
    }
    if (align == 2) {
      const int iconX = showIcon ? (rightX - battW) : (rightX - battW);
      drawBatteryRight(renderer, Rect{iconX, batteryY, battW, battH}, mode);
    } else if (align == 1) {
      const int iconX = centerX - groupW / 2;
      drawBatteryLeft(renderer, Rect{iconX, batteryY, battW, battH}, mode);
    } else {
      drawBatteryLeft(renderer, Rect{leftX, batteryY, battW, battH}, mode);
    }
  };

  auto drawBatteryWarningAt = [&](int align) {
    if (!showBatteryWarning || warnPctShow < 0) return;
    char warnBuf[48];
    // "Battery 15% · Charge Soon"
    snprintf(warnBuf, sizeof(warnBuf), "Battery %d%% · %s", warnPctShow, tr(STR_CHARGE_SOON));
    constexpr int kFont = SMALL_FONT_ID;
    const int lineH = renderer.getLineHeight(kFont);
    const int textY = baseTopY + std::max(2, (metrics.statusBarVerticalMargin - lineH) / 2);
    const int maxW = std::max(40, (screenW / 2) - 16);
    const std::string vis = renderer.truncatedText(kFont, warnBuf, maxW);
    const int tw = renderer.getTextWidth(kFont, vis.c_str());
    int textX = centerX - tw / 2;
    if (align == 0)
      textX = leftX;
    else if (align == 2)
      textX = rightX - tw;
    renderer.drawText(kFont, textX, textY, vis.c_str());
  };

  auto drawSlot = [&](uint8_t content, int align) {
    using S = CrossPointSettings::SYSTEM_STATUS_SLOT;
    if (content == S::SYS_SLOT_BATTERY) {
      drawBatteryAt(align);
    } else if (content == S::SYS_SLOT_CLOCK) {
      drawClockAt(align);
    } else if (content == S::SYS_SLOT_BATTERY_WARNING) {
      // In Status Bar settings preview always paint the sample; live only when low.
      // When force preview and this slot is empty of warning placement, still
      // paint so user sees layout (handled by slot assignment + force flag).
      if (forceBatteryWarningPreview || showBatteryWarning) {
        drawBatteryWarningAt(align);
      }
    }
  };

  drawSlot(SETTINGS.systemStatusBarLeft, 0);
  drawSlot(SETTINGS.systemStatusBarMiddle, 1);
  drawSlot(SETTINGS.systemStatusBarRight, 2);

  // Settings preview: if no slot has Battery Warning yet, sample it in Middle
  // so the user still sees the message while configuring.
  if (forceBatteryWarningPreview && !SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY_WARNING)) {
    drawBatteryWarningAt(/*align=*/1);
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                                int selectedIndex) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
  // Focus is bold-only (same as drawList). Always size dialog against BOLD so the
  // selected row never widens past the frame.
  (void)metrics.optionPopupOptionFontBold;

  const int itemSpacing = metrics.optionPopupItemSpacing;
  const int innerPadding = metrics.optionPopupInnerPadding;
  const int selectionHPadding = metrics.optionPopupSelectionHPadding;
  const int selectionVPadding = metrics.optionPopupSelectionVPadding;

  const int optionLineHeight = renderer.getLineHeight(optionFontId);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = optionLineHeight + selectionVPadding * 2;
  const int rowStep = rowHeight + itemSpacing;

  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  for (const auto& opt : options) {
    int w = renderer.getTextWidth(optionFontId, opt.c_str(), EpdFontFamily::BOLD);
    if (w > maxTextWidth) maxTextWidth = w;
  }

  const int optionCount = static_cast<int>(options.size());
  // Leave room for button hints below and a small top margin so the dialog never
  // spills past the screen edges (long pickers like Customize Reader slots).
  const int topMargin = 8;
  const int bottomMargin = metrics.buttonHintsHeight + 8;
  const int maxDialogH = std::max(rowHeight + innerPadding * 2 + titleLineHeight + metrics.optionPopupTitleGap,
                                  pageHeight - topMargin - bottomMargin);
  const int chromeH = innerPadding * 2 + titleLineHeight + metrics.optionPopupTitleGap;
  const int maxListH = std::max(rowHeight, maxDialogH - chromeH);
  int visibleCount = optionCount;
  if (rowStep > 0) {
    // (listH + spacing) / step fits N rows with N-1 gaps.
    visibleCount = std::max(1, (maxListH + itemSpacing) / rowStep);
  }
  if (visibleCount > optionCount) visibleCount = optionCount;

  int firstVisible = 0;
  if (optionCount > visibleCount) {
    firstVisible = selectedIndex - (visibleCount - 1) / 2;
    if (firstVisible < 0) firstVisible = 0;
    if (firstVisible > optionCount - visibleCount) firstVisible = optionCount - visibleCount;
  }

  const int listHeight = visibleCount > 0 ? rowHeight * visibleCount + itemSpacing * std::max(0, visibleCount - 1) : 0;
  const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2) * 12 / 10,
                               pageWidth - metrics.optionPopupDialogSideMargin * 2);
  const int contentHeight = titleLineHeight + metrics.optionPopupTitleGap + listHeight;
  const int dialogH = std::min(contentHeight + innerPadding * 2, maxDialogH);
  const int dialogX = (pageWidth - dialogW) / 2;
  const int availH = pageHeight - topMargin - bottomMargin;
  const int dialogY = topMargin + std::max(0, (availH - dialogH) / 2);

  const int frameThickness = metrics.popupFrameThickness;
  const int frameRadius = metrics.popupCornerRadius;

  if (frameRadius > 0) {
    renderer.fillRoundedRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                             dialogH + frameThickness * 2, frameRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(dialogX, dialogY, dialogW, dialogH, frameRadius, Color::Black);
    renderer.fillRoundedRect(dialogX + frameThickness, dialogY + frameThickness, dialogW - frameThickness * 2,
                             dialogH - frameThickness * 2,
                             frameRadius - frameThickness > 0 ? frameRadius - frameThickness : 0, Color::White);
  } else {
    renderer.fillRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                      dialogH + frameThickness * 2, true);
    renderer.fillRect(dialogX, dialogY, dialogW, dialogH, false);
  }

  int y = dialogY + innerPadding;

  renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
  y += titleLineHeight;

  if (metrics.optionPopupTitleSeparator) {
    const int sepY = y + metrics.optionPopupTitleGap / 2;
    renderer.drawLine(dialogX + innerPadding, sepY, dialogX + dialogW - innerPadding, sepY, true);
  }

  y += metrics.optionPopupTitleGap;

  const int itemRectX = dialogX + innerPadding;
  // Reserve a few pixels on the right for a scroll bar when the list is windowed.
  const bool showScroll = optionCount > visibleCount;
  constexpr int kScrollReserve = 10;
  const int itemRectW = dialogW - innerPadding * 2 - (showScroll ? kScrollReserve : 0);
  // Selection chips left residual on FAST e-ink; bold weight is the shared focus cue.
  (void)metrics.optionPopupSelectionRadius;
  (void)metrics.optionPopupSelectionLight;
  (void)metrics.optionPopupDrawAllRows;

  for (int vis = 0; vis < visibleCount; vis++) {
    const int i = firstVisible + vis;
    const int itemY = y + vis * rowStep;
    const bool selected = (i == selectedIndex);
    const char* labelText = options[i].c_str();
    const auto optionStyle = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    const int textW = renderer.getTextWidth(optionFontId, labelText, optionStyle);
    const int textY = itemY + (rowHeight - optionLineHeight) / 2;
    const int textX = itemRectX + (itemRectW - textW) / 2;
    renderer.drawText(optionFontId, textX, textY, labelText, /*black=*/true, optionStyle);
  }

  if (showScroll) {
    const int trackX = dialogX + dialogW - innerPadding - 3;
    const int trackTop = y;
    const int trackH = listHeight;
    renderer.drawLine(trackX, trackTop, trackX, trackTop + trackH - 1, true);
    const int thumbH = std::max(8, (trackH * visibleCount) / optionCount);
    const int maxTravel = std::max(0, trackH - thumbH);
    const int thumbY =
        trackTop + (optionCount > visibleCount ? (maxTravel * firstVisible) / (optionCount - visibleCount) : 0);
    renderer.fillRect(trackX - 2, thumbY, 4, thumbH, true);
  }
}
