#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}
}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  // System top chrome: Left / Middle / Right (Display → Status Bar).
  drawSystemStatusBar(renderer, rect.y, nullptr);

  constexpr int kHeaderRuleThickness = 3;
  const int ruleY = rect.y + rect.height - kHeaderRuleThickness;
  const int sideReserve = systemStatusSideReserve(renderer);
  const int maxTitleWidth = std::max(40, rect.width - sideReserve * 2);

  if (title && title[0] != '\0') {
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    // Center title in the free band between real status chrome (clock/battery)
    // and the bottom rule — not batteryBarHeight (layout reserve, too tall on Lyra).
    const int chromeBottom =
        rect.y + BaseTheme::kTopChromeBatteryY + 6 + LyraMetrics::values.batteryHeight;
    const int titleY = chromeBottom + std::max(0, (ruleY - chromeBottom - lineH) / 2);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kHeaderRuleThickness, true);
  }

  if (subtitle) {
    const int maxSubtitleWidth =
        std::max(40, rect.width / 3 - LyraMetrics::values.contentSidePadding);
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    const int subY = rect.y + std::max(2, LyraMetrics::values.batteryBarHeight / 2 - 4);
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth, subY,
                      truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  int rightSpace = LyraMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - LyraMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  // Same thickness as the Settings header rule under the title (drawHeader).
  constexpr int kRuleThickness = 3;
  // Same air as Manage Fonts Preview label band (lineH + 10).
  constexpr int kBandExtraPad = 10;

  if (tabs.empty()) {
    const int ruleY = rect.y + rect.height - kRuleThickness;
    renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kRuleThickness, true);
    return;
  }

  // Equal-width slots; pick largest font so every label (e.g. "Download") fits.
  const int slotW = std::max(1, rect.width / static_cast<int>(tabs.size()));
  // Pill uses horizontal padding around the label — leave that margin inside the slot.
  const int maxTextW = std::max(8, slotW - 2 * hPaddingInSelection - 2);
  static constexpr int kFontCandidates[] = {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};
  int fontId = SMALL_FONT_ID;
  for (const int candidate : kFontCandidates) {
    bool fits = true;
    for (const auto& tab : tabs) {
      if (renderer.getTextWidth(candidate, tab.label, EpdFontFamily::REGULAR) > maxTextW) {
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
  // Header rule is the top edge of this rect; bottom rule at rect bottom.
  // Preview-style band so tab labels do not crowd either rule.
  const int availH = std::max(1, rect.height - kRuleThickness);
  const int contentBandH = lineHeight + kBandExtraPad;
  const int contentTop = rect.y + std::max(0, (availH - contentBandH) / 2);
  const int textY = contentTop + (contentBandH - lineHeight) / 2;

  // Active tab: bold only (no underline / black pill). Clear enough to show
  // which category is open when focus is on a list row below.
  for (size_t i = 0; i < tabs.size(); i++) {
    const auto& tab = tabs[i];
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

bool LyraTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  (void)renderer;
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height || x < rect.x || x >= rect.x + rect.width) {
    return false;
  }

  const int slotW = std::max(1, rect.width / static_cast<int>(tabs.size()));
  index = std::min(static_cast<int>(tabs.size()) - 1, (x - rect.x) / slotW);
  return true;
}

int LyraTheme::getListRowStep(bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  return rowHeight;
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const int rowStep = getListRowStep(hasSubtitle);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& rowApplied) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  const int maxRowWidth = std::max(0, contentWidth - LyraMetrics::values.contentSidePadding * 2);

  int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize = 0;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  constexpr int kRadioR = 5;
  constexpr int kRadioReserve = kRadioR * 2 + 8;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;
    const bool applied = rowApplied && rowApplied(i);
    if (applied) rowTextWidth -= kRadioReserve;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
        valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());

    // Selection:
    // - Settings rows with a value (On/Off, enum): black chip on the value.
    // - Settings rows with no value (Clock folder, other actions): black chip on the name.
    // - File browser: black chip over icon + name.
    const bool isSelected = (i == selectedIndex);
    const bool valueChip = isSelected && highlightValue && !valueText.empty();
    const bool namePill = isSelected && !valueChip;
    const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
    // Single-line rows: center label/icon in the row. Subtitle rows (Recent Books):
    // center the title+author block so they no longer stack on top of each other.
    int textDrawY = itemY + std::max(0, (rowHeight - titleLineH) / 2);
    int subtitleDrawY = textDrawY;
    std::string subtitleDrawn;
    if (rowSubtitle != nullptr) {
      const std::string subtitleRaw = rowSubtitle(i);
      if (!subtitleRaw.empty()) {
        subtitleDrawn = renderer.truncatedText(SMALL_FONT_ID, subtitleRaw.c_str(), rowTextWidth);
        const int subtitleLineH = renderer.getLineHeight(SMALL_FONT_ID);
        constexpr int kTitleSubtitleGap = 2;
        const int blockH = titleLineH + kTitleSubtitleGap + subtitleLineH;
        const int blockY = itemY + std::max(0, (rowHeight - blockH) / 2);
        textDrawY = blockY;
        subtitleDrawY = blockY + titleLineH + kTitleSubtitleGap;
      }
    }
    const int iconDrawY = itemY + std::max(0, (rowHeight - iconSize) / 2);
    const int iconX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;

    if (namePill) {
      int textRight = textX + titleWidth;
      if (!subtitleDrawn.empty()) {
        textRight = std::max(textRight, textX + renderer.getTextWidth(SMALL_FONT_ID, subtitleDrawn.c_str()));
      }
      // Union of icon bounds + title/subtitle + padding. Always reserve the icon
      // slot when a rowIcon callback is present (folder, book, file, image, …).
      const int selX = rect.x + LyraMetrics::values.contentSidePadding;
      int contentRight = textRight + hPaddingInSelection;
      if (rowIcon != nullptr) {
        contentRight = std::max(contentRight, iconX + iconSize + hPaddingInSelection);
      }
      int selW = contentRight - selX;
      selW = std::min(selW, maxRowWidth);
      selW = std::max(selW, std::max(hPaddingInSelection * 2 + 12, cornerRadius * 2 + 4));
      // Full row height so 24×24 file/folder icons are never clipped top/bottom.
      const int selH = rowHeight;
      const int selRadius = std::min(cornerRadius, std::min(selW, selH) / 2);
      renderer.fillRoundedRect(selX, itemY, selW, selH, selRadius, Color::Black);
    }

    // White text on black name pill; otherwise normal black text.
    renderer.drawText(UI_10_FONT_ID, textX, textDrawY, item.c_str(), !namePill);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && !isSelected) {
      for (int py = textDrawY; py < textDrawY + titleLineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        // White ink on the black selection pill (folder, book, file, text, image).
        renderer.drawIcon(iconBitmap, iconX, iconDrawY, iconSize, /*black=*/!namePill);
      }
    }

    if (!subtitleDrawn.empty()) {
      renderer.drawText(SMALL_FONT_ID, textX, subtitleDrawY, subtitleDrawn.c_str(), !namePill);
    }

    // Settings-style black value chip (On / Off / enum) when the row has a value.
    // Do not use a value chip for applied-state (radio circle) — focus stays on the name.
    if (!valueText.empty()) {
      if (valueChip) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      const int valueLineH = renderer.getLineHeight(UI_10_FONT_ID);
      const int valueY = itemY + std::max(0, (rowHeight - valueLineH) / 2);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - LyraMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), !valueChip);
    }

    if (applied) {
      const int cx = rect.x + contentWidth - LyraMetrics::values.contentSidePadding - kRadioR;
      const int cy = itemY + rowHeight / 2;
      // Radio sits on the right. Name-pill focus only blacks the left label —
      // white ink there was invisible. Only invert when a right-side value chip
      // is under the radio (On/Off style rows).
      const bool ink = !valueChip;
      for (int dy = -kRadioR; dy <= kRadioR; ++dy) {
        for (int dx = -kRadioR; dx <= kRadioR; ++dx) {
          if (dx * dx + dy * dy <= kRadioR * kRadioR) renderer.drawPixel(cx + dx, cy + dy, ink);
        }
      }
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  // Same front-button chrome as BaseTheme / Minimal / Dashboard / dictionary.
  BaseTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                    const BookReadingStats* stats, float progressPercent,
                                    const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)stats;
  (void)progressPercent;
  (void)globalStats;
  (void)currentChapterTitle;
  const int tileWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = LyraMetrics::values.homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = LyraMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                LyraMetrics::values.homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                        LyraMetrics::values.homeCoverHeight, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (LyraMetrics::values.homeCoverHeight / 3), coverWidth,
                          2 * LyraMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32);
      }

      const int coverX = tileX + hPaddingInSelection;
      const int coverY = tileY + hPaddingInSelection;
      coverBufferStored =
          storeCoverBuffer(coverX, coverY, coverWidth, LyraMetrics::values.homeCoverHeight);
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = LyraMetrics::values.contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - LyraMetrics::values.verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                              LyraMetrics::values.homeCoverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, LyraMetrics::values.homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + LyraMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const int totalBlockHeight = titleBlockHeight + authorHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + LyraMetrics::values.verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - LyraMetrics::values.contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + LyraMetrics::values.contentSidePadding,
                         rect.y + i * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing), tileWidth,
                         LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    // Black selection chip (same language as Settings). Light gray showed cover
    // ghosting badly on e-ink when opening the home menu over a book cover.
    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::Black);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        // White ink on black chip; black ink otherwise.
        renderer.drawIcon(iconBitmap, textX, textY, mainMenuIconSize, /*black=*/!selected);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, !selected);
  }
}
