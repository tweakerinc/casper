#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/FinishedBooks.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;
  // Opened via Bare long-press Library (physical Confirm still held): ignore all
  // nav/select until every open/nav button is released, then require a new press.
  bool awaitOpenButtonRelease = false;

  // Recents list (excludes books under /read). Or read-books view from SD scan.
  enum class ViewMode : uint8_t { Recents, ReadBooks };
  ViewMode viewMode = ViewMode::Recents;

  std::vector<RecentBook> recentBooks;
  std::vector<FinishedBooks::FinishedBookEntry> readBooks;

  // Recents row 0 = "Show Read Books" only when move-to-read is on and /read has ΓëÑ1 book.
  bool showReadBooksRow() const;

  void loadRecentBooks();
  void loadReadBooks();
  void reloadAfterBookAction();

  int listRowCount() const;
  // Maps selector index to book index; returns -1 for the "Show Read Books" row.
  int bookIndexForSelector(size_t sel) const;
  void activateSelector();

  // CrossInk-style long-press menu; side effects stay in the menu activity.
  void showBookActionMenu(size_t bookIndex, bool ignoreInitialConfirmRelease = false);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};