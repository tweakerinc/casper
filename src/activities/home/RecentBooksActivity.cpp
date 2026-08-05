#include "RecentBooksActivity.h"
#include "util/UiGhostPolicy.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FinishedBooks.h"

namespace {
// Match HomeActivity Read long-press (was 1000 → 500 → 300).
constexpr unsigned long LONG_PRESS_MS = 300;

// True while any button that can open this screen or activate a row is held.
// Bare long-press Library uses physical Confirm; release must not open a book.
bool anyOpenOrNavButtonHeld(const MappedInputManager& input) {
  using B = MappedInputManager::Button;
  if (input.isPressed(B::Confirm) || input.isPressed(B::Back) || input.isPressed(B::Left) ||
      input.isPressed(B::Right) || input.isPressed(B::Up) || input.isPressed(B::Down)) {
    return true;
  }
  return input.isFrontButtonPressed(HalGPIO::BTN_BACK) || input.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         input.isFrontButtonPressed(HalGPIO::BTN_LEFT) || input.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  for (const auto& book : books) {
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }
    // Finished books live under /read — show them only in "Show Read Books".
    if (FinishedBooks::isInFinishedFolder(book.path)) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::loadReadBooks() {
  FinishedBooks::listFinishedBooks(readBooks);
}

int RecentBooksActivity::listRowCount() const {
  if (viewMode == ViewMode::ReadBooks) {
    return static_cast<int>(readBooks.size());
  }
  return (kShowReadRow ? 1 : 0) + static_cast<int>(recentBooks.size());
}

int RecentBooksActivity::bookIndexForSelector(const size_t sel) const {
  if (viewMode == ViewMode::ReadBooks) {
    return static_cast<int>(sel);
  }
  if (kShowReadRow) {
    if (sel == 0) return -1;  // "Show Read Books" row
    return static_cast<int>(sel - 1);
  }
  return static_cast<int>(sel);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  viewMode = ViewMode::Recents;
  loadRecentBooks();
  loadReadBooks();  // so we know whether read folder has content (optional UX)
  selectorIndex = 0;
  longPressFired = false;
  // Opened via Bare long-press Library (Confirm still held): wait for full release
  // so the release edge does not open the first book.
  awaitOpenButtonRelease = anyOpenOrNavButtonHeld(mappedInput);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  readBooks.clear();
}

void RecentBooksActivity::activateSelector() {
  if (viewMode == ViewMode::Recents) {
    const int bi = bookIndexForSelector(selectorIndex);
    if (bi < 0) {
      viewMode = ViewMode::ReadBooks;
      loadReadBooks();
      selectorIndex = 0;
      requestUpdate(true);
      return;
    }
    if (bi < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[static_cast<size_t>(bi)].path.c_str());
      onSelectBook(recentBooks[static_cast<size_t>(bi)].path);
    }
    return;
  }
  // Read books view
  if (selectorIndex < readBooks.size()) {
    LOG_DBG("RBA", "Selected read book: %s", readBooks[selectorIndex].path.c_str());
    onSelectBook(readBooks[selectorIndex].path);
  }
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (awaitOpenButtonRelease) {
    if (anyOpenOrNavButtonHeld(mappedInput)) {
      return;
    }
    awaitOpenButtonRelease = false;
    return;
  }

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm: book action menu (not on "Show Read Books" row).
  {
    const int bi = bookIndexForSelector(selectorIndex);
    const bool onBook = (viewMode == ViewMode::ReadBooks)
                            ? (selectorIndex < readBooks.size())
                            : (bi >= 0 && bi < static_cast<int>(recentBooks.size()));
    if (onBook && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      longPressFired = true;
      const size_t menuIndex = (viewMode == ViewMode::ReadBooks) ? selectorIndex : static_cast<size_t>(bi);
      showBookActionMenu(menuIndex, true);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (listRowCount() > 0) {
      activateSelector();
      return;
    }
  }

  int touchSel = static_cast<int>(selectorIndex);
  const int rows = listRowCount();
  const auto listTouch = handleListTouch(touchSel, rows, contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) {
      activateSelector();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::ReadBooks) {
      viewMode = ViewMode::Recents;
      loadRecentBooks();
      selectorIndex = 0;
      requestUpdate(true);
      return;
    }
    onGoHome();
    return;
  }

  const int listSize = listRowCount();
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::reloadAfterBookAction() {
  if (viewMode == ViewMode::ReadBooks) {
    loadReadBooks();
    if (readBooks.empty()) {
      selectorIndex = 0;
    } else if (selectorIndex >= readBooks.size()) {
      selectorIndex = readBooks.size() - 1;
    }
  } else {
    loadRecentBooks();
    const int rows = listRowCount();
    if (rows <= 0) {
      selectorIndex = 0;
    } else if (static_cast<int>(selectorIndex) >= rows) {
      selectorIndex = static_cast<size_t>(rows - 1);
    }
  }
  requestUpdate(true);
}

void RecentBooksActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  std::string title;
  std::string path;
  if (viewMode == ViewMode::ReadBooks) {
    if (bookIndex >= readBooks.size()) return;
    title = readBooks[bookIndex].title;
    path = readBooks[bookIndex].path;
  } else {
    if (bookIndex >= recentBooks.size()) return;
    title = recentBooks[bookIndex].title;
    path = recentBooks[bookIndex].path;
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, title, path,
                                                  /*includeRemoveFromRecents=*/viewMode == ViewMode::Recents,
                                                  ignoreInitialConfirmRelease),
      [this, path](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Book action result missing");
          requestUpdate();
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::Open:
            onSelectBook(path);
            return;
          case FileBrowserAction::Delete:
          case FileBrowserAction::RemoveFromRecents:
            reloadAfterBookAction();
            return;
          default:
            // Mark finished / unfinished may have moved the file.
            reloadAfterBookAction();
            return;
        }
      });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* header = (viewMode == ViewMode::ReadBooks) ? tr(STR_SHOW_READ_BOOKS) : tr(STR_MENU_RECENT_BOOKS);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const int rows = listRowCount();
  if (rows == 0) {
    const char* empty = (viewMode == ViewMode::ReadBooks) ? tr(STR_NO_READ_BOOKS) : tr(STR_NO_RECENT_BOOKS);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, empty);
  } else if (viewMode == ViewMode::ReadBooks) {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, readBooks.size(), selectorIndex,
                 [this](int index) { return readBooks[static_cast<size_t>(index)].title; },
                 [](int) { return std::string{}; }, nullptr);
  } else {
    // Row 0 = Show Read Books; following rows = recent titles.
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<size_t>(rows), selectorIndex,
        [this](int index) -> std::string {
          if (kShowReadRow && index == 0) {
            return tr(STR_SHOW_READ_BOOKS);
          }
          const int bi = kShowReadRow ? index - 1 : index;
          if (bi >= 0 && bi < static_cast<int>(recentBooks.size())) {
            return recentBooks[static_cast<size_t>(bi)].title;
          }
          return {};
        },
        [this](int index) -> std::string {
          if (kShowReadRow && index == 0) {
            return {};
          }
          const int bi = kShowReadRow ? index - 1 : index;
          if (bi >= 0 && bi < static_cast<int>(recentBooks.size())) {
            return recentBooks[static_cast<size_t>(bi)].author;
          }
          return {};
        },
        nullptr);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  // In Read view, Back returns to Recents (hint still says Home via mapLabels — OK for hardware).
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  UiGhostPolicy::displayMenuFrame(renderer);
}
