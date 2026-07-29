#include "FileBrowserActionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include <Epub.h>
#include <FsHelpers.h>
#include <Xtc.h>

#include "BookActions.h"
#include "BookDescriptionActivity.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Match Bare footer / larger chrome: UI_12 reads better than UI_10 on the action list.
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kMenuFontId = UI_12_FONT_ID;
constexpr int kTitleMaxLines = 2;
constexpr int kCompactTitleY = 14;
constexpr int kTitleLineGap = 1;
constexpr int kBatteryTextReserveWidth = 90;
constexpr int kMenuRowPadY = 10;

// True while any button that can open this menu or navigate it is still held.
// Dashboard long-press uses physical Read (BTN_RIGHT); recents uses Confirm.
// NavNext includes Right, so a held Read would continuous-scroll the list.
bool anyOpenOrNavButtonHeld(const MappedInputManager& input) {
  using B = MappedInputManager::Button;
  if (input.isPressed(B::Confirm) || input.isPressed(B::Back) || input.isPressed(B::Left) ||
      input.isPressed(B::Right) || input.isPressed(B::Up) || input.isPressed(B::Down)) {
    return true;
  }
  // Raw front slots (dashboard Read is hardware Right, independent of remaps).
  return input.isFrontButtonPressed(HalGPIO::BTN_BACK) || input.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         input.isFrontButtonPressed(HalGPIO::BTN_LEFT) || input.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}
}  // namespace

FileBrowserActionActivity::FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string title, std::string bookPath,
                                                     const bool includeRemoveFromRecents,
                                                     const bool openedFromLongPress)
    : Activity("FileBrowserAction", renderer, mappedInput),
      title(std::move(title)),
      bookPath(std::move(bookPath)),
      includeRemoveFromRecents(includeRemoveFromRecents),
      bookMode(true),
      awaitOpenButtonRelease(openedFromLongPress) {
  rebuildItems();
}

FileBrowserActionActivity::FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string title, std::vector<MenuItem> items,
                                                     const bool openedFromLongPress)
    : Activity("FileBrowserAction", renderer, mappedInput),
      title(std::move(title)),
      bookMode(false),
      items(std::move(items)),
      awaitOpenButtonRelease(openedFromLongPress) {}

void FileBrowserActionActivity::rebuildItems() {
  if (!bookMode) return;
  items = BookActions::buildBookActionItems(bookPath, includeRemoveFromRecents);
  if (selectedIndex >= static_cast<int>(items.size())) {
    selectedIndex = std::max(0, static_cast<int>(items.size()) - 1);
  }
}

void FileBrowserActionActivity::stayInMenu() {
  // After a nested confirmation / toast, swallow residual presses so we do not
  // immediately re-activate an item or cancel via a stale Back/Confirm edge.
  awaitOpenButtonRelease = true;
  rebuildItems();
  requestUpdate();
}

void FileBrowserActionActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void FileBrowserActionActivity::activateSelected() {
  if (items.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const FileBrowserAction action = items[static_cast<size_t>(selectedIndex)].action;

  // Custom menus (clippings, etc.): return the choice; parent does the work.
  if (!bookMode) {
    setResult(FileBrowserActionResult{static_cast<int>(action)});
    finish();
    return;
  }

  switch (action) {
    case FileBrowserAction::Open:
      setResult(FileBrowserActionResult{static_cast<int>(action)});
      finish();
      return;

    case FileBrowserAction::Description: {
      std::string desc = BookActions::loadBookDescription(bookPath);
      startActivityForResult(
          std::make_unique<BookDescriptionActivity>(renderer, mappedInput, title, std::move(desc)),
          [this](const ActivityResult&) { stayInMenu(); });
      return;
    }

    case FileBrowserAction::ReadingStats: {
      std::string cachePath = BookReadingStats::cachePathForBook(bookPath);
      if (cachePath.empty() && FsHelpers::hasEpubExtension(bookPath)) {
        cachePath = Epub(bookPath, "/.crosspoint").getCachePath();
      } else if (cachePath.empty() && FsHelpers::hasXtcExtension(bookPath)) {
        Xtc xtc(bookPath, "/.crosspoint");
        if (xtc.load()) cachePath = xtc.getCachePath();
      }
      BookReadingStats stats = BookReadingStats::loadForBook(bookPath);
      const float progress = stats.getProgressPercent();
      const GlobalReadingStats global = GlobalReadingStats::load();
      const GlobalReadingStats aggregated = GlobalReadingStats::loadAggregated(global);
      startActivityForResult(
          std::make_unique<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0u,
                                              global, aggregated, /*returnToHomeOnExit=*/false),
          [this](const ActivityResult&) { stayInMenu(); });
      return;
    }

    case FileBrowserAction::ResetPace:
      if (BookActions::resetReadingPace(bookPath)) {
        BookActions::drawToast(renderer, tr(STR_RESET_READING_PACE));
        delay(800);
      }
      selectedIndex = 0;
      stayInMenu();
      return;

    case FileBrowserAction::ToggleCompleted: {
      bool completed = false;
      if (BookActions::toggleBookCompleted(bookPath, title, completed)) {
        BookActions::drawToast(renderer, completed ? tr(STR_MARK_FINISHED) : tr(STR_MARK_UNFINISHED));
        delay(800);
      }
      selectedIndex = 0;
      stayInMenu();
      return;
    }

    case FileBrowserAction::RemoveFromRecents:
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
          [this](const ActivityResult& confirmation) {
            if (confirmation.isCancelled) {
              stayInMenu();
              return;
            }
            RECENT_BOOKS.removeByPath(bookPath);
            setResult(FileBrowserActionResult{static_cast<int>(FileBrowserAction::RemoveFromRecents)});
            finish();
          });
      return;

    case FileBrowserAction::Delete:
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::string(tr(STR_DELETE)) + "? ", title),
          [this](const ActivityResult& confirmation) {
            if (confirmation.isCancelled) {
              stayInMenu();
              return;
            }
            BookActions::clearFileMetadata(bookPath);
            if (!Storage.remove(bookPath.c_str())) {
              LOG_ERR("BookAction", "Failed to delete file: %s", bookPath.c_str());
              stayInMenu();
              return;
            }
            RECENT_BOOKS.removeByPath(bookPath);
            setResult(FileBrowserActionResult{static_cast<int>(FileBrowserAction::Delete)});
            finish();
          });
      return;

    case FileBrowserAction::DeleteStats:
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(
              renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), title),
          [this](const ActivityResult& confirmation) {
            if (!confirmation.isCancelled) {
              if (BookActions::deleteBookStats(bookPath)) {
                BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                delay(800);
              }
              selectedIndex = 0;
            }
            stayInMenu();
          });
      return;

    case FileBrowserAction::DeleteCache:
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(
              renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), title),
          [this](const ActivityResult& confirmation) {
            if (!confirmation.isCancelled) {
              if (BookActions::clearBookCache(bookPath)) {
                BookActions::drawToast(renderer, tr(STR_DELETE_CACHE));
                delay(800);
              }
              // Leave focus on a safe row so a stray Confirm cannot re-open this dialog.
              selectedIndex = 0;
            }
            stayInMenu();
          });
      return;
  }
}

void FileBrowserActionActivity::loop() {
  // After long-press open or nested confirm/toast: wait until buttons are up, then
  // drain residual edges on *this* frame and return (never activate).
  //
  // Falling through used to re-fire Confirm → Delete Cache confirm loop.
  // Returning without draining used to drop a real Back (edge expired next frame).
  if (awaitOpenButtonRelease) {
    if (anyOpenOrNavButtonHeld(mappedInput)) {
      return;
    }
    awaitOpenButtonRelease = false;

    const bool backToLeave = mappedInput.wasReleased(MappedInputManager::Button::Back);
    // Drain Confirm / front residuals so they cannot re-open the just-closed action.
    (void)mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    (void)mappedInput.wasPressed(MappedInputManager::Button::Confirm);
    (void)mappedInput.getReleasedFrontButton();

    if (backToLeave) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    // Quiet frame: no activateSelected, no nav. Next frame is normal menu input.
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });
}

void FileBrowserActionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int titleX = metrics.contentSidePadding;
  const int titleMaxWidth = std::max(0, pageWidth - titleX - metrics.contentSidePadding - kBatteryTextReserveWidth);
  const auto titleLines =
      renderer.wrappedText(kTitleFontId, title.c_str(), titleMaxWidth, kTitleMaxLines, EpdFontFamily::BOLD);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int titleBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight +
                               std::max(0, static_cast<int>(titleLines.size()) - 1) * kTitleLineGap;
  const bool tallHeader = metrics.headerHeight > 60;
  const int titleY = metrics.topPadding + (tallHeader ? metrics.batteryBarHeight + 3 : kCompactTitleY);
  const int titleBottomPadding = tallHeader ? 8 : 4;
  const int actionHeaderHeight =
      std::max(metrics.headerHeight, titleY - metrics.topPadding + titleBlockHeight + titleBottomPadding);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, actionHeaderHeight}, "");

  for (int i = 0; i < static_cast<int>(titleLines.size()); ++i) {
    renderer.drawText(kTitleFontId, titleX, titleY + i * (titleLineHeight + kTitleLineGap), titleLines[i].c_str(), true,
                      EpdFontFamily::BOLD);
  }

  const int contentTop = metrics.topPadding + actionHeaderHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  // Custom list so we can use UI_12 (shared drawList is hardcoded to UI_10).
  const int lineH = renderer.getLineHeight(kMenuFontId);
  const int rowH = lineH + kMenuRowPadY * 2;
  const int sidePad = metrics.contentSidePadding;
  const int pageItems = std::max(1, contentHeight / rowH);
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int listCount = static_cast<int>(items.size());

  for (int i = pageStart; i < listCount && i < pageStart + pageItems; ++i) {
    const int rowY = contentTop + (i - pageStart) * rowH;
    const bool selected = (i == selectedIndex);
    const std::string label = I18N.get(items[static_cast<size_t>(i)].labelId);
    const int maxLabelW = std::max(20, pageWidth - sidePad * 2 - 16);
    const std::string drawn = renderer.truncatedText(kMenuFontId, label.c_str(), maxLabelW, EpdFontFamily::REGULAR);
    const int tw = renderer.getTextWidth(kMenuFontId, drawn.c_str(), EpdFontFamily::REGULAR);
    const int pillW = std::min(pageWidth - sidePad * 2, tw + 16);
    const int pillX = sidePad;
    if (selected) {
      renderer.fillRoundedRect(pillX, rowY, pillW, rowH, 6, Color::Black);
    }
    // Center label in the black pill (pad was pushing text toward the bottom).
    const int textY = rowY + std::max(0, (rowH - lineH) / 2);
    renderer.drawText(kMenuFontId, pillX + 8, textY, drawn.c_str(), !selected, EpdFontFamily::REGULAR);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
