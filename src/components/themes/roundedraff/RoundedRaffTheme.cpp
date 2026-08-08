#include "RoundedRaffTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr int kCoverRadius = 18;
constexpr int kMenuRadius = 30;
constexpr int kBottomRadius = 15;
constexpr int kRowRadius = 20;
constexpr int kInteractiveInsetX = 20;
constexpr int kSelectableRowGap = 6;
constexpr int kTitleFontId = UI_12_FONT_ID;     // Requested main title size: 12px
constexpr int kSubtitleFontId = SMALL_FONT_ID;  // Requested subtitle size: 8px
constexpr int kGuideFontId = SMALL_FONT_ID;     // Closest available to requested 6px

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  const int barW = RoundedRaffMetrics::values.scrollBarWidth;
  const int barX = rect.x + rect.width - RoundedRaffMetrics::values.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(10, (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
  const int thumbY = barY + (clampedStart * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

}  // namespace
int coverWidth = 0;

void RoundedRaffTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                                  const char* subtitle) const {
  (void)subtitle;
  // Home screen header is custom-rendered in drawRecentBookCover — still draw
  // system status chrome so top chrome matches reader/dashboard.
  drawSystemStatusBar(renderer, rect.y, nullptr);

  if (title != nullptr && title[0] != '\0') {
    // Title below battery/clock row so it never overlaps the time.
    const int sideReserve = systemStatusSideReserve(renderer);
    const int maxTitleWidth = std::max(40, rect.width - sideReserve * 2);
    const int titleBelowChromeY = rect.y + RoundedRaffMetrics::values.batteryBarHeight + 3;
    auto headerTitle = renderer.truncatedText(kTitleFontId, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawCenteredText(kTitleFontId, titleBelowChromeY, headerTitle.c_str(), true, EpdFontFamily::BOLD);
    // Full-width rule under the title (matched by drawTabBar bottom rule).
    constexpr int kHeaderRuleThickness = 1;
    renderer.drawLine(rect.x, rect.y + rect.height - kHeaderRuleThickness, rect.x + rect.width - 1,
                      rect.y + rect.height - kHeaderRuleThickness, kHeaderRuleThickness, true);
  }
}

void RoundedRaffTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                  bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int slotWidth = rect.width / static_cast<int>(tabs.size());
  const int tabY = rect.y + 4;
  const int tabHeight = rect.height - 12;

  for (size_t i = 0; i < tabs.size(); i++) {
    const int slotX = rect.x + static_cast<int>(i) * slotWidth;
    const int tabX = slotX + 4;
    const int tabWidth = slotWidth - 8;
    const auto& tab = tabs[i];

    if (tab.selected) {
      renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, 18, selected ? Color::Black : Color::DarkGray);
    }

    const int textWidth = renderer.getTextWidth(kTitleFontId, tab.label, EpdFontFamily::BOLD);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(kTitleFontId)) / 2;
    renderer.drawText(kTitleFontId, textX, textY, tab.label, !(tab.selected), EpdFontFamily::BOLD);
  }

  // Full-width divider between tabs and setting rows (match Lyra/BaseTheme header rule).
  constexpr int kRuleThickness = 1;
  const int ruleY = rect.y + rect.height - kRuleThickness;
  renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kRuleThickness, true);
}

bool RoundedRaffTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                         const int x, const int y, int& index) const {
  (void)renderer;
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height || x < rect.x || x >= rect.x + rect.width) {
    return false;
  }

  const int slotWidth = std::max(1, rect.width / static_cast<int>(tabs.size()));
  index = std::min(static_cast<int>(tabs.size()) - 1, (x - rect.x) / slotWidth);
  return true;
}

void RoundedRaffTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                           const BookReadingStats* stats, float progressPercent,
                                           const GlobalReadingStats* globalStats,
                                           const char* currentChapterTitle) const {
  (void)stats;
  (void)progressPercent;
  (void)globalStats;
  (void)currentChapterTitle;
  const int tileWidth = rect.width - 2 * RoundedRaffMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = RoundedRaffMetrics::values.homeCoverHeight * 0.6;
  }
  const int imgY = tileY + (tileHeight - RoundedRaffMetrics::values.homeCoverHeight) / 2;
  const int tileX = RoundedRaffMetrics::values.contentSidePadding;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(coverPath, RoundedRaffMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                RoundedRaffMetrics::values.homeCoverHeight);
            renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                                   RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                                   Color::LightGray);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRoundedRect(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                               RoundedRaffMetrics::values.homeCoverHeight, 1, kCoverRadius, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + (tileWidth - coverWidth) / 2, imgY + (RoundedRaffMetrics::values.homeCoverHeight / 3),
                          coverWidth, 2 * RoundedRaffMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + (tileWidth - coverWidth) / 2 + 24, imgY + 24, 32);
        renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                               RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                               Color::LightGray);
      }

      const int coverX = tileX + (tileWidth - coverWidth) / 2;
      coverBufferStored = storeCoverBuffer(coverX, imgY, coverWidth, RoundedRaffMetrics::values.homeCoverHeight);
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    renderer.fillRoundedRect(tileX, tileY, tileWidth, imgY - tileY, kRowRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, imgY, (tileWidth - coverWidth) / 2, RoundedRaffMetrics::values.homeCoverHeight,
                            Color::LightGray);
    renderer.fillRectDither(tileX + (tileWidth + coverWidth) / 2, imgY, (tileWidth - coverWidth) / 2,
                            RoundedRaffMetrics::values.homeCoverHeight, Color::LightGray);
    renderer.fillRoundedRect(tileX, imgY + RoundedRaffMetrics::values.homeCoverHeight, tileWidth,
                             tileHeight - (imgY - tileY + RoundedRaffMetrics::values.homeCoverHeight), kRowRadius,
                             false, false, true, true, Color::LightGray);
  } else {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kRowRadius, Color::LightGray);
    renderer.drawCenteredText(kTitleFontId, rect.y + rect.height / 2 - renderer.getLineHeight(kTitleFontId) / 2,
                              tr(STR_NO_OPEN_BOOK));
  }
}

void RoundedRaffTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                      const std::function<std::string(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowHeight = renderer.getLineHeight(kTitleFontId) + 20;  // 10px top + 10px bottom
  const int rowGap = kSelectableRowGap;
  const int rowStep = rowHeight + rowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelectedIndex = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelectedIndex / pageItems) * pageItems;
  const int menuTop = rect.y;
  const int textLineHeight = renderer.getLineHeight(kTitleFontId);
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const std::string label = buttonLabel(i);
    const int rowY = menuTop + (i - pageStartIndex) * rowStep;
    constexpr int kRowPaddingX = 40;  // 20px L/R
    const int maxLabelWidth = std::max(0, menuMaxWidth - kRowPaddingX);
    const std::string truncatedLabel =
        renderer.truncatedText(kTitleFontId, label.c_str(), maxLabelWidth, EpdFontFamily::BOLD);
    const int rowWidth = std::min(
        menuMaxWidth, renderer.getTextWidth(kTitleFontId, truncatedLabel.c_str(), EpdFontFamily::BOLD) + kRowPaddingX);
    const bool isSelected = selectedIndex == i;
    // Hollow outline only — no solid black chip (e-ink residual on FAST scroll).
    renderer.drawRoundedRect(rowX, rowY, rowWidth, rowHeight, isSelected ? 2 : 1, kMenuRadius, /*black=*/true);
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + kInteractiveInsetX;
    renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), /*black=*/true,
                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  drawScrollBar(renderer, rect, buttonCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                     int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;

  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth - 1, lineY, thickness, true);
    return;
  }

  constexpr int hPadding = 8;
  const int lineW = textWidth + hPadding * 2;
  const int lineStart = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineStart, lineY, lineStart + lineW - 1, lineY, thickness, true);
}

int RoundedRaffTheme::getListRowStep(bool hasSubtitle) const {
  const int rowHeight =
      hasSubtitle ? RoundedRaffMetrics::values.listWithSubtitleRowHeight : RoundedRaffMetrics::values.listRowHeight;
  return rowHeight + kSelectableRowGap;
}

int RoundedRaffTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  return std::max(1, contentHeight / getListRowStep(hasSubtitle));
}

void RoundedRaffTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                const std::function<std::string(int index)>& rowTitle,
                                const std::function<std::string(int index)>& rowSubtitle,
                                const std::function<UIIcon(int index)>& rowIcon,
                                const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                const std::function<bool(int index)>& rowDimmed,
                                const std::function<bool(int index)>& rowApplied) const {
  (void)rowIcon;
  (void)highlightValue;
  (void)rowDimmed;
  (void)rowApplied;
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(kSubtitleFontId);
  constexpr int subtitleTopPadding = 10;
  constexpr int subtitleBottomPadding = 10;
  constexpr int subtitleInterLineGap = 4;
  const int subtitleRowHeight =
      subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
  const int rowHeight = hasSubtitle ? subtitleRowHeight : RoundedRaffMetrics::values.listRowHeight;
  const int rowStep = rowHeight + kSelectableRowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = std::max(0, selectedIndex / pageItems) * pageItems;

  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowWidth = rect.width - sidePadding * 2;
  constexpr int kRowPaddingX = kInteractiveInsetX * 2;  // match drawButtonMenu L/R inset

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int rowY = rect.y + (i % pageItems) * rowStep;
    const bool isSelected = i == selectedIndex;

    constexpr int kMinTitleWidth = 40;
    constexpr int kMinValueGap = kInteractiveInsetX;
    int textAreaWidth = rowWidth - kRowPaddingX;
    int valueW = 0;
    std::string truncatedValue;
    if (rowValue) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValueWidth = std::max(0, rowWidth - kRowPaddingX - kMinValueGap - kMinTitleWidth);
        if (maxValueWidth > 0) {
          truncatedValue =
              renderer.truncatedText(kTitleFontId, valueText.c_str(), maxValueWidth, EpdFontFamily::REGULAR);
          valueW = renderer.getTextWidth(kTitleFontId, truncatedValue.c_str(), EpdFontFamily::REGULAR);
          textAreaWidth = std::max(0, textAreaWidth - valueW - kMinValueGap);
        }
      }
    }

    const std::string title =
        renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);
    const int titleW = renderer.getTextWidth(kTitleFontId, title.c_str(), EpdFontFamily::BOLD);
    int pillContentW = titleW;
    std::string subtitle;
    if (hasSubtitle) {
      const std::string subtitleRaw = rowSubtitle(i);
      if (!subtitleRaw.empty()) {
        subtitle = renderer.truncatedText(kSubtitleFontId, subtitleRaw.c_str(), textAreaWidth, EpdFontFamily::REGULAR);
        pillContentW =
            std::max(pillContentW, renderer.getTextWidth(kSubtitleFontId, subtitle.c_str(), EpdFontFamily::REGULAR));
      }
    }

    // Hollow outline pill hugs the name — no black fill.
    int pillW = std::min(rowWidth, pillContentW + kRowPaddingX);
    pillW = std::max(pillW, kRowPaddingX + 12);
    if (isSelected) {
      renderer.drawRoundedRect(rowX, rowY, pillW, rowHeight, 2, kRowRadius, /*black=*/true);
    }

    const auto focusStyle = isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (valueW > 0) {
      renderer.drawText(kTitleFontId, rowX + rowWidth - kInteractiveInsetX - valueW,
                        rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, truncatedValue.c_str(),
                        /*black=*/true, EpdFontFamily::REGULAR);
    }

    if (hasSubtitle) {
      if (subtitle.empty()) {
        const int centeredTitleY = rowY + (rowHeight - titleLineHeight) / 2;
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, centeredTitleY, title.c_str(), /*black=*/true,
                          focusStyle);
      } else {
        const int titleY = rowY + subtitleTopPadding;
        const int subtitleY = titleY + titleLineHeight + subtitleInterLineGap;
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, titleY, title.c_str(), /*black=*/true, focusStyle);
        renderer.drawText(kSubtitleFontId, rowX + kInteractiveInsetX, subtitleY, subtitle.c_str(), /*black=*/true,
                          EpdFontFamily::REGULAR);
      }
    } else {
      renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX,
                        rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, title.c_str(), /*black=*/true,
                        focusStyle);
    }
  }

  drawScrollBar(renderer, rect, itemCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                       const char* btn4) const {
  // Match Dashboard / dictionary / settings: shared 80×buttonHintsHeight chrome.
  BaseTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
}
