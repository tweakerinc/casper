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

#include "CasperSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bluetooth.h"
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
      case UIIcon::Bluetooth:
        return BluetoothIcon;
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

  // 1px: thick rules ghosted on FAST settings navigation.
  constexpr int kHeaderRuleThickness = 1;
  const int ruleY = rect.y + rect.height - kHeaderRuleThickness;
  const int sideReserve = systemStatusSideReserve(renderer);
  const int maxTitleWidth = std::max(40, rect.width - sideReserve * 2);

  if (title && title[0] != '\0') {
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    // Center title in the free band between real status chrome (clock/battery)
    // and the bottom rule — not batteryBarHeight (layout reserve, too tall on Lyra).
    const int chromeBottom = rect.y + BaseTheme::kTopChromeBatteryY + 6 + LyraMetrics::values.batteryHeight;
    const int titleY = chromeBottom + std::max(0, (ruleY - chromeBottom - lineH) / 2);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, ruleY, rect.x + rect.width - 1, ruleY, kHeaderRuleThickness, true);
  }

  if (subtitle) {
    const int maxSubtitleWidth = std::max(40, rect.width / 3 - LyraMetrics::values.contentSidePadding);
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
  constexpr int kRuleThickness = 1;
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

namespace {
// Shared with Bare/Penumbra (they inherit LyraTheme::drawList).
// Single-line row height for short titles/folders. Wrapped titles use a tighter
// line step than full advanceY (body leading is too loose for list UI) and only
// those rows grow taller so short names stay dense.
constexpr int kLyraTitleSubtitleGap = 2;
constexpr int kLyraRowPad = 10;

int lyraTitleLineStep(const GfxRenderer& renderer, const int titleFont, const int nLines) {
  const int advanceY = renderer.getLineHeight(titleFont);
  if (nLines <= 1) return advanceY;
  // Bring the second line up toward the first (~70% of body leading).
  return std::max(18, (advanceY * 7) / 10);
}

int lyraTitleBlockHeight(const GfxRenderer& renderer, const int titleFont, const int nLines) {
  const int advanceY = renderer.getLineHeight(titleFont);
  const int step = lyraTitleLineStep(renderer, titleFont, nLines);
  // Last line still needs full advanceY for descenders.
  return (nLines <= 1) ? advanceY : ((nLines - 1) * step + advanceY);
}

int lyraListRowHeightForLines(const GfxRenderer& renderer, const bool hasSubtitle, const int nTitleLines) {
  const int titleFont = SETTINGS.getMenuListFontId();
  int contentH = lyraTitleBlockHeight(renderer, titleFont, std::max(1, nTitleLines));
  if (hasSubtitle) {
    contentH += kLyraTitleSubtitleGap + renderer.getLineHeight(SMALL_FONT_ID);
  }
  const int computed = contentH + kLyraRowPad;
  const int baseline = hasSubtitle ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  return std::max(baseline, computed);
}

int lyraListRowHeight(const GfxRenderer& renderer, const bool hasSubtitle) {
  // Navigation / touch use single-line height; paint path sizes per item.
  return lyraListRowHeightForLines(renderer, hasSubtitle, 1);
}
}  // namespace

int LyraTheme::getListRowStep(bool hasSubtitle) const {
  // Approximate without GfxRenderer (touch hit + page-size math). Must stay ≥
  // painted single-line row height or taps land one row too low (Size 10→12).
  // Painting still measures per item for multi-line wrap.
  int rowHeight = hasSubtitle ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  // Match compute path: larger menu list fonts grow title line height.
  switch (SETTINGS.menuFontSize) {
    case CasperSettings::MENU_FONT_XSMALL:
      rowHeight = std::max(26, rowHeight - 4);
      break;
    case CasperSettings::MENU_FONT_SMALL:
      rowHeight = std::max(28, rowHeight - 2);
      break;
    case CasperSettings::MENU_FONT_MEDIUM:
      rowHeight += 8;  // ~14 pt Source Serif list titles
      break;
    case CasperSettings::MENU_FONT_LARGE:
      rowHeight += 14;  // ~16 pt
      break;
    default:
      break;
  }
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
                         const std::function<bool(int index)>& rowApplied,
                         const std::function<bool(int index)>& rowCentered) const {
  // Icons no longer drawn — free horizontal space for long book titles.
  (void)rowIcon;

  const bool hasSubtitleCb = (rowSubtitle != nullptr);
  const int singleRowH = lyraListRowHeight(renderer, hasSubtitleCb);
  int pageItems = singleRowH > 0 ? std::max(1, rect.height / singleRowH) : 1;

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

  const int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  const int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;

  const int titleFont = SETTINGS.getMenuListFontId();
  const int titleMaxLines = SETTINGS.getMenuListTitleMaxLines();
  constexpr int kRadioR = 5;
  constexpr int kRadioReserve = kRadioR * 2 + 8;

  // Draw all items with cumulative Y so wrapped titles can be taller without
  // forcing empty double-height on short names/folders.
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int itemY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    int rowTextWidth = textWidth;
    const bool applied = rowApplied && rowApplied(i);
    if (applied) rowTextWidth -= kRadioReserve;

    int valueWidth = 0;
    std::string valueText;
    const bool centered = rowCentered && rowCentered(i);
    if (!centered && rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        valueText = renderer.truncatedText(titleFont, valueText.c_str(), maxListValueWidth);
        valueWidth = renderer.getTextWidth(titleFont, valueText.c_str()) + hPaddingInSelection;
        rowTextWidth -= valueWidth;
      }
    }

    const auto itemName = rowTitle(i);
    std::vector<std::string> titleLines =
        renderer.wrappedText(titleFont, itemName.c_str(), rowTextWidth, titleMaxLines, EpdFontFamily::REGULAR);
    if (titleLines.empty()) titleLines.emplace_back("");
    if (static_cast<int>(titleLines.size()) >= titleMaxLines) {
      titleLines.back() = renderer.truncatedText(titleFont, titleLines.back().c_str(), rowTextWidth);
    }
    const int nTitleLines = static_cast<int>(titleLines.size());
    const int lineStep = lyraTitleLineStep(renderer, titleFont, nTitleLines);
    const int titleBlockH = lyraTitleBlockHeight(renderer, titleFont, nTitleLines);

    // Focus: bold only — no outline/fill (minimal residual on FAST scroll).
    const bool isSelected = (i == selectedIndex);
    (void)highlightValue;
    (void)maxRowWidth;

    std::string subtitleDrawn;
    int subtitleLineH = 0;
    if (!centered && rowSubtitle != nullptr) {
      const std::string subtitleRaw = rowSubtitle(i);
      if (!subtitleRaw.empty()) {
        subtitleDrawn = renderer.truncatedText(SMALL_FONT_ID, subtitleRaw.c_str(), rowTextWidth);
        subtitleLineH = renderer.getLineHeight(SMALL_FONT_ID);
      }
    }

    const int rowHeight = centered ? renderer.getLineHeight(UI_12_FONT_ID) * 3
                                   : lyraListRowHeightForLines(renderer, !subtitleDrawn.empty() || hasSubtitleCb, nTitleLines);
    // Stop if this taller row would leave the list area (keep at least one row).
    if (i > pageStartIndex && itemY + rowHeight > rect.y + rect.height) {
      break;
    }

    const int blockH = titleBlockH + (subtitleDrawn.empty() ? 0 : (kLyraTitleSubtitleGap + subtitleLineH));
    const int textDrawY = itemY + (centered ? 0 : std::max(0, (rowHeight - blockH) / 2));
    const int subtitleDrawY = textDrawY + titleBlockH + kLyraTitleSubtitleGap;

    const auto focusStyle = (isSelected && !centered) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (centered) {
      const int headerFont = UI_12_FONT_ID;
      for (size_t li = 0; li < titleLines.size(); ++li) {
        const int ly = textDrawY + static_cast<int>(li) * lineStep;
        renderer.drawCenteredText(headerFont, ly, titleLines[li].c_str(), /*black=*/true, EpdFontFamily::BOLD);
      }
    } else {
      for (size_t li = 0; li < titleLines.size(); ++li) {
        const int ly = textDrawY + static_cast<int>(li) * lineStep;
        renderer.drawText(titleFont, textX, ly, titleLines[li].c_str(), /*black=*/true, focusStyle);
      }
    }

    if (rowDimmed && rowDimmed(i) && !isSelected && !centered) {
      const int dimH = renderer.getLineHeight(titleFont);
      for (size_t li = 0; li < titleLines.size(); ++li) {
        const int ly = textDrawY + static_cast<int>(li) * lineStep;
        const int tw = renderer.getTextWidth(titleFont, titleLines[li].c_str());
        for (int py = ly; py < ly + dimH; py++)
          for (int px = textX; px < textX + tw; px++)
            if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
      }
    }

    if (!subtitleDrawn.empty()) {
      renderer.drawText(SMALL_FONT_ID, textX, subtitleDrawY, subtitleDrawn.c_str(), /*black=*/true);
    }

    if (!valueText.empty()) {
      const int valueLineH = renderer.getLineHeight(titleFont);
      const int valueY = itemY + std::max(0, (rowHeight - valueLineH) / 2);
      renderer.drawText(titleFont, rect.x + contentWidth - LyraMetrics::values.contentSidePadding - valueWidth, valueY,
                        valueText.c_str(), /*black=*/true, focusStyle);
    }

    if (applied) {
      const int cx = rect.x + contentWidth - LyraMetrics::values.contentSidePadding - kRadioR;
      const int cy = itemY + rowHeight / 2;
      for (int dy = -kRadioR; dy <= kRadioR; ++dy) {
        for (int dx = -kRadioR; dx <= kRadioR; ++dx) {
          if (dx * dx + dy * dy <= kRadioR * kRadioR) renderer.drawPixel(cx + dx, cy + dy, /*black=*/true);
        }
      }
    }

    itemY += rowHeight;
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
      coverBufferStored = storeCoverBuffer(coverX, coverY, coverWidth, LyraMetrics::values.homeCoverHeight);
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
  // Home Menu: centered text-only list (no icons). Optional rowIcon kept for
  // any legacy callers that still pass one.
  const bool drawIcons = static_cast<bool>(rowIcon);
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = rect.y + i * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing);
    const bool selected = selectedIndex == i;
    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const auto style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileY + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    if (drawIcons) {
      const int tileX = rect.x + LyraMetrics::values.contentSidePadding;
      int textX = tileX + 16;
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, textY, mainMenuIconSize, /*black=*/true);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, /*black=*/true, style);
    } else {
      const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, style);
      const int textX = rect.x + (rect.width - textWidth) / 2;
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, /*black=*/true, style);
    }
  }
}
