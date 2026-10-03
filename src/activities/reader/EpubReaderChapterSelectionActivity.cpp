#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return !epub?0:expanded_?int(sections_.size()):epub->getTocItemsCount(); }
BookMetadataCache::TocEntry EpubReaderChapterSelectionActivity::selectedItem(int index) const {
  if(!epub||index<0||index>=getTotalItems())return {};
  if(!expanded_)return epub->getTocItem(index);
  const auto entry=sections_.at(index);
  if(entry.toc>=0)return epub->getTocItem(entry.toc);
  BookMetadataCache::TocEntry item;item.spineIndex=entry.spine;item.level=1;
  item.title=rivulet::sectionlabel::read(irDir_.c_str(),entry.spine);
  if(item.title.empty())item.title=std::string(tr(STR_SECTION_PREFIX))+std::to_string(entry.spine+1);
  return item;
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // A sparse publisher TOC must not make the omitted book sections unreachable.
  // Preserve the original TOC behavior for ordinary comprehensive navigation.
  if(epub->getTocItemsCount()*2<epub->getSpineItemsCount()) {
    expanded_=sections_.build(epub->getSpineItemsCount(),epub->getTocItemsCount(),
                             [this](int i){return epub->getTocItem(i).spineIndex;});
  }
  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if(expanded_){selectorIndex=0;for(size_t i=0;i<sections_.size();++i)if(sections_.at(i).spine==currentSpineIndex){selectorIndex=int(i);break;}}
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if(totalItems<=0)return;

  auto selectChapter = [this] {
    const auto tocItem = selectedItem(selectorIndex);
    if (tocItem.spineIndex < 0 || tocItem.spineIndex >= epub->getSpineItemsCount()) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
      finish();
    }
  };

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  switch (handleListTouch(selectorIndex, totalItems, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      selectChapter();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectChapter();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = selectedItem(index);
                 std::string indent(std::clamp(int(item.level) - 1,0,8) * 2, ' ');
                 return indent + item.title;
               });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  UiGhostPolicy::displayMenuFrame(renderer);
}
