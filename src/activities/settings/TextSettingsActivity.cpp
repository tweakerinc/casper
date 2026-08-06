#include "TextSettingsActivity.h"
#include "util/UiGhostPolicy.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Tab labels for Font | Size | Layout | Style (Download Fonts is a Font-list row).
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_FONT, StrId::STR_SIZE, StrId::STR_LAYOUT, StrId::STR_STYLE};

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }

  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}

int findCurrentFontSizeIndex(uint8_t fontSize, size_t listSize) {
  return fontSize < listSize ? fontSize : 0;  // default SMALL (Lexend Deca 12)
}

constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab)
    : Activity("TextSettings", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

void TextSettingsActivity::onEnter() {
  Activity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  // Match Settings: tab bar sits directly under the header so the shared
  // drawHeader / drawTabBar thick rules sandwich the tabs.
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.tabBarHeight;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;

  rebuildFontList();

  // Point sizes matching built-in faces (12 / 14 / 16 / 18), not Small/Medium/…
  sizes_.clear();
  sizes_.reserve(CrossPointSettings::FONT_SIZE_COUNT);
  sizes_.push_back({"12", static_cast<uint8_t>(CrossPointSettings::SMALL)});
  sizes_.push_back({"14", static_cast<uint8_t>(CrossPointSettings::MEDIUM)});
  sizes_.push_back({"16", static_cast<uint8_t>(CrossPointSettings::LARGE)});
  sizes_.push_back({"18", static_cast<uint8_t>(CrossPointSettings::EXTRA_LARGE)});

  currentFamilyIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  currentSizeIndex_ = findCurrentFontSizeIndex(SETTINGS.fontSize, sizes_.size());
  // Nav ring: 0 = tab bar, 1..N = list rows. Open focused on the tab bar so the
  // user can pick Font / Size / Layout / … before diving into a list item.
  std::fill(std::begin(selectedIndex_), std::end(selectedIndex_), 0);

  // Reader menu / Settings open this with Confirm (or a remapped key). That
  // release must not count as "cycle tab" on the first frame.
  armAwaitOpenButtonRelease(/*force=*/true);

  // One scrub on open; scrolling/tabs stay FAST (no periodic HALF while navigating).
  UiGhostPolicy::requestHardScrub();
  requestUpdate();
}

void TextSettingsActivity::armAwaitOpenButtonRelease(const bool force) {
  using B = MappedInputManager::Button;
  const bool held = mappedInput.isPressed(B::Back) || mappedInput.isPressed(B::Confirm) ||
                    mappedInput.isPressed(B::Left) || mappedInput.isPressed(B::Right) ||
                    mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down) ||
                    mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
                    mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
                    mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) ||
                    mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
  awaitOpenButtonRelease_ = force || held;
}

void TextSettingsActivity::onExit() { Activity::onExit(); }

TextSettingsActivity::PaneGeometry TextSettingsActivity::paneGeometry() const {
  // Fixed panes so tab changes never shift layout:
  // tabs → settings list box (~half) → Preview + sample (~half).
  // Half/half gives Font list more visible rows when users install SD fonts.
  constexpr int kBoxInset = 2;
  const int tabTop = metrics_.topPadding + metrics_.headerHeight;
  const int gap = metrics_.verticalSpacing;
  const int listTop = tabTop + metrics_.tabBarHeight + gap;
  const int usableBottom = afterHeader + usableHeight;  // above button hints

  const int contentSpan = std::max(0, usableBottom - listTop);
  // Split remaining vertical space roughly 50/50 (gap sits between the panes).
  const int half = std::max(0, (contentSpan - gap) / 2);
  // Never shorter than the densest fixed tab (Style) so those rows never clip.
  const int rowStep = std::max(1, GUI.getListRowStep(false));
  const int styleRows = static_cast<int>(StyleRow::Count);
  const int minList = styleRows * rowStep + kBoxInset * 2;
  const int listHeight = std::max(minList, half);

  const int previewTop = listTop + listHeight + gap;
  const int previewHeight = std::max(0, usableBottom - previewTop);
  return {previewTop, previewHeight, tabTop, listTop, listHeight};
}

bool TextSettingsActivity::handleTouch() {
  // Inert on non-touch boards: the events simply never fire.
  int tx = 0;
  int ty = 0;
  const auto geo = paneGeometry();
  const int listCount = currentListSize();

  // TODO: this tab-bar touch pass duplicates SettingsActivity's
  // this will have to be refactored when a handleTabBarTouch() helper exist
  // (similar to handleListTouch)
  auto buildTabs = [this]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(static_cast<int>(Tab::Count));
    for (int t = 0; t < static_cast<int>(Tab::Count); t++) {
      tabs.push_back({I18N.get(TAB_NAME_IDS[t]), tab_ == static_cast<Tab>(t)});
    }
    return tabs;
  };
  int tabHit = -1;
  if ((mappedInput.wasScreenTouchDown(tx, ty) || mappedInput.wasScreenTapped(tx, ty)) &&
      GUI.tabIndexFromPoint(renderer, Rect{0, geo.tabTop, renderer.getScreenWidth(), metrics_.tabBarHeight},
                            buildTabs(), tx, ty, tabHit)) {
    if (tab_ != static_cast<Tab>(tabHit)) {
      tab_ = static_cast<Tab>(tabHit);
      selectedIndex() = 0;
      requestUpdate();
    }
    return true;
  }

  // Match render() inset so list hits stay inside the settings box border.
  constexpr int kBoxInset = 2;
  const int listTop = geo.listTop + kBoxInset;
  const int listHeight = std::max(0, geo.listHeight - kBoxInset * 2);

  int row = std::max(0, selectedIndex() - 1);
  switch (handleListTouch(row, listCount, listTop, listHeight, /*hasSubtitle=*/false)) {
    case ListTouchResult::Activated:
      selectedIndex() = row + 1;
      activateRow(row);
      return true;
    case ListTouchResult::Consumed:
      selectedIndex() = row + 1;
      return true;
    case ListTouchResult::None:
      break;
  }

  // Vertical swipe pages the list; ring includes tab bar at index 0.
  const int pageItems = GUI.getListPageItems(listHeight, false);
  const int ringSize = listCount + 1;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex() =
        selectedIndex() == 0 ? 1 : ButtonNavigator::nextPageIndex(selectedIndex(), ringSize, pageItems);
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex() = ButtonNavigator::previousPageIndex(selectedIndex(), ringSize, pageItems);
    requestUpdate();
    return true;
  }

  return false;
}

void TextSettingsActivity::loop() {
  if (awaitOpenButtonRelease_) {
    using B = MappedInputManager::Button;
    const bool held = mappedInput.isPressed(B::Back) || mappedInput.isPressed(B::Confirm) ||
                      mappedInput.isPressed(B::Left) || mappedInput.isPressed(B::Right) ||
                      mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down) ||
                      mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
                      mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
                      mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) ||
                      mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
    if (held) {
      return;
    }
    // Drain residual edges from the open gesture so they cannot cycle tabs or exit.
    (void)mappedInput.wasPressed(B::Back);
    (void)mappedInput.wasReleased(B::Back);
    (void)mappedInput.wasPressed(B::Confirm);
    (void)mappedInput.wasReleased(B::Confirm);
    (void)mappedInput.wasPressed(B::Left);
    (void)mappedInput.wasReleased(B::Left);
    (void)mappedInput.wasPressed(B::Right);
    (void)mappedInput.wasReleased(B::Right);
    (void)mappedInput.wasPressed(B::Up);
    (void)mappedInput.wasReleased(B::Up);
    (void)mappedInput.wasPressed(B::Down);
    (void)mappedInput.wasReleased(B::Down);
    (void)mappedInput.getReleasedFrontButton();
    (void)mappedInput.getPressedFrontButton();
    awaitOpenButtonRelease_ = false;
    return;
  }

  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;  // picker owns input while open

  // Finish on release, not press. When this screen is opened from the reader
  // menu, a press-to-finish leaves a Back release for the reader — and the
  // reader treats any Back release as "go home" after reflow. Match CrossPoint.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex() == 0) {
      // Stay on the tab bar so Select can cycle Font / Size / Layout / Style
      // without dropping into the first list row each time.
      switchTab(1, /*focusTabBar=*/true);
      return;
    }

    activateRow(selectedIndex() - 1);
    return;
  }

  if (handleTouch()) return;

  const int listCount = currentListSize();
  // Front Up/Down: within-tab list ring (0 = tab label, 1..N = rows).
  // Side Up/Down: always previous/next tab, even when a list row is focused.
  const int ringSize = listCount + 1;

  auto moveListNext = [this, ringSize] {
    selectedIndex() = ButtonNavigator::nextIndex(selectedIndex(), ringSize);
    requestUpdate();
  };
  auto moveListPrev = [this, ringSize] {
    selectedIndex() = ButtonNavigator::previousIndex(selectedIndex(), ringSize);
    requestUpdate();
  };
  // Preserve relative focus when flipping tabs (tab bar stays tab bar; list stays list).
  auto moveTabNext = [this] {
    const bool onTabs = selectedIndex() == 0;
    const int listPos = selectedIndex();
    switchTab(1, /*focusTabBar=*/onTabs);
    if (!onTabs) {
      const int n = currentListSize();
      if (n <= 0) {
        selectedIndex() = 0;
      } else if (listPos > n) {
        selectedIndex() = n;
      } else {
        selectedIndex() = listPos;
      }
      requestUpdate();
    }
  };
  auto moveTabPrev = [this] {
    const bool onTabs = selectedIndex() == 0;
    const int listPos = selectedIndex();
    switchTab(-1, /*focusTabBar=*/onTabs);
    if (!onTabs) {
      const int n = currentListSize();
      if (n <= 0) {
        selectedIndex() = 0;
      } else if (listPos > n) {
        selectedIndex() = n;
      } else {
        selectedIndex() = listPos;
      }
      requestUpdate();
    }
  };

  buttonNavigator_.onRelease(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator_.onRelease(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);
  buttonNavigator_.onContinuous(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator_.onContinuous(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);

  buttonNavigator_.onRelease(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator_.onRelease(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);
  buttonNavigator_.onContinuous(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator_.onContinuous(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);
}

void TextSettingsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_TEXT_SETTINGS));
  // Header uses STR_TEXT_SETTINGS ("Manage Fonts"); Download Fonts is last Font-list row.

  const auto geo = paneGeometry();

  // Tab bar directly under header (same chrome as SettingsActivity).
  const bool onTabBar = selectedIndex() == 0;
  std::vector<TabInfo> tabs;
  tabs.reserve(static_cast<int>(Tab::Count));
  for (int t = 0; t < static_cast<int>(Tab::Count); t++) {
    tabs.push_back({I18N.get(TAB_NAME_IDS[t]), tab_ == static_cast<Tab>(t)});
  }
  GUI.drawTabBar(renderer, Rect{0, geo.tabTop, pageWidth, metrics_.tabBarHeight}, tabs, onTabBar);

  // Settings list box (~half of content band; same rect every tab).
  const int side = metrics_.contentSidePadding;
  const int boxW = pageWidth - side * 2;
  const Rect settingsBox{side, geo.listTop, boxW, geo.listHeight};
  if (settingsBox.width > 0 && settingsBox.height > 0) {
    // Rounded outline — same language as lifetime stats cards / date fields.
    constexpr int kBoxRadius = 8;
    constexpr int kBoxStroke = 2;
    renderer.drawRoundedRect(settingsBox.x, settingsBox.y, settingsBox.width, settingsBox.height, kBoxStroke,
                             kBoxRadius, true);
  }
  // Inset list so selection highlight stays inside the border.
  constexpr int kBoxInset = 2;
  const Rect listRect{settingsBox.x + kBoxInset, settingsBox.y + kBoxInset,
                      std::max(0, settingsBox.width - kBoxInset * 2),
                      std::max(0, settingsBox.height - kBoxInset * 2)};
  const int selectedItem = selectedIndex() - 1;
  const char* confirmLabel = tr(STR_SELECT);

  switch (tab_) {
    case Tab::Family:
      // Focus on name; applied font = radio; Download Fonts row uses ">" affordance.
      GUI.drawList(
          renderer, listRect, static_cast<int>(fonts_.size()), selectedItem,
          [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
          [this](int index) -> std::string {
            return fonts_[index].isDownloadAction ? std::string(">") : std::string();
          },
          true, nullptr,
          [this](int index) {
            return !fonts_[index].isDownloadAction && index == currentFamilyIndex_;
          });
      if (onTabBar) confirmLabel = tr(STR_SIZE);
      break;

    case Tab::Size:
      GUI.drawList(
          renderer, listRect, static_cast<int>(sizes_.size()), selectedItem,
          [this](int index) { return sizes_[index].name; }, nullptr, nullptr, nullptr, false, nullptr,
          [this](int index) { return index == currentSizeIndex_; });
      if (onTabBar) confirmLabel = tr(STR_LAYOUT);
      break;

    case Tab::Layout: {
      constexpr int LAYOUT_ROWS = static_cast<int>(LayoutRow::Count);
      static constexpr StrId ROW_NAME_IDS[LAYOUT_ROWS] = {StrId::STR_LINE_SPACING, StrId::STR_EXTRA_SPACING,
                                                          StrId::STR_ALIGNMENT, StrId::STR_SCREEN_MARGIN};
      GUI.drawList(
          renderer, listRect, LAYOUT_ROWS, selectedItem,
          [](int index) { return std::string(I18N.get(ROW_NAME_IDS[index])); }, nullptr, nullptr,
          [this](int index) { return layoutValueText(index); }, true);
      if (onTabBar)
        confirmLabel = tr(STR_STYLE);
      else  // Extra Paragraph Spacing toggles; the rest open a picker
        confirmLabel = (selectedItem == static_cast<int>(LayoutRow::ParaSpacing)) ? tr(STR_TOGGLE) : tr(STR_SELECT);
      break;
    }

    case Tab::Style: {
      constexpr int STYLE_ROWS = static_cast<int>(StyleRow::Count);
      static constexpr StrId ROW_NAME_IDS[STYLE_ROWS] = {
          StrId::STR_BIONIC_READING, StrId::STR_GUIDE_READING, StrId::STR_HYPHENATION, StrId::STR_EMBEDDED_STYLE,
          StrId::STR_TEXT_AA};
      GUI.drawList(
          renderer, listRect, STYLE_ROWS, selectedItem,
          [](int index) { return std::string(I18N.get(ROW_NAME_IDS[index])); }, nullptr, nullptr,
          [this](int index) { return styleValueText(index); }, true);
      confirmLabel = onTabBar ? tr(STR_FONT) : tr(STR_TOGGLE);
      break;
    }

    default:
      break;
  }

  const char* familyName =
      (currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()) &&
       !fonts_[currentFamilyIndex_].isDownloadAction)
          ? fonts_[currentFamilyIndex_].name.c_str()
          : "";
  const char* sizeName = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                             ? sizes_[currentSizeIndex_].name.c_str()
                             : "";
  // Unboxed full-width preview: double-line "Preview" header + reader-accurate sample.
  textsettings::renderPreview(renderer, previewLayout_, geo.previewTop, geo.previewHeight, familyName, sizeName,
                              focusedRowHasNoPreview() ? tr(STR_NOT_IN_PREVIEW) : nullptr);

  // Front Up/Down: list. Side: switch tabs. Confirm on tab bar also advances tab.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  UiGhostPolicy::displayMenuFrame(renderer);
}

// Font switching runs on the main task from loop(), which deliberately holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, and the render task walks that same object inside the preview's
// prewarmCache() — so without this lock a font switch can free the mini glyph
// arrays out from under prewarmStyle() (crash: null s.miniGlyphs mid-read/sort).
void TextSettingsActivity::rebuildFontList() {
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0) + 1);
  fonts_.push_back({I18N.get(StrId::STR_SOURCE_SERIF_4), true, static_cast<uint8_t>(CrossPointSettings::SOURCESERIF4)});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::BITTER)});
  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }
  // Last row: open catalog / Wi‑Fi download (keeps tab bar to four labels so type stays large).
  fonts_.push_back({I18N.get(StrId::STR_DOWNLOAD_FONTS), false, 0, /*isDownloadAction=*/true});
}

void TextSettingsActivity::applyFamily(int listIndex) {
  if (listIndex < 0 || listIndex >= static_cast<int>(fonts_.size()) || fonts_[listIndex].isDownloadAction) return;
  RenderLock lock;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);  // unloads the previously resident SD font
    currentFamilyIndex_ = listIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
      currentFamilyIndex_ = listIndex;
    }
  }
}

void TextSettingsActivity::activateRow(int row) {
  switch (tab_) {
    case Tab::Family:
      if (row < 0 || row >= static_cast<int>(fonts_.size())) break;
      if (fonts_[row].isDownloadAction) {
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 sdFontSystem.refreshIfDirty();
                                 rebuildFontList();
                                 currentFamilyIndex_ =
                                     findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
                                 requestUpdate();
                               });
        break;
      }
      if (row != currentFamilyIndex_) {
        applyFamily(row);
        requestUpdate();
      }
      break;
    case Tab::Size:
      if (row != currentSizeIndex_) {
        applySize(row);
        requestUpdate();
      }
      break;
    case Tab::Layout:
      confirmLayoutRow(row);
      break;
    case Tab::Style:
      confirmStyleRow(row);
      break;
    default:
      break;
  }
}

// Same RenderLock rationale as applyFamily(): a size change reloads the SD font
// file, which frees and replaces the SdCardFont the render task may be reading.
void TextSettingsActivity::applySize(int listIndex) {
  RenderLock lock;

  currentSizeIndex_ = listIndex;
  SETTINGS.fontSize = sizes_[listIndex].settingIndex;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      requestUpdate();
      break;
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) { SETTINGS.lineSpacing = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment,
                        [](int idx) { SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur,
                        [](int idx) { SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP); });
      requestUpdate();
      break;
    }

    default:
      break;
  }
}

std::string TextSettingsActivity::layoutValueText(int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: {
      const uint8_t v = SETTINGS.lineSpacing;
      return v < std::size(LINE_SPACING_IDS) ? I18N.get(LINE_SPACING_IDS[v]) : I18N.get(StrId::STR_NORMAL);
    }
    case LayoutRow::ParaSpacing:
      return SETTINGS.extraParagraphSpacing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case LayoutRow::Alignment: {
      const uint8_t v = SETTINGS.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(SETTINGS.screenMargin);

    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::BionicReading:
      SETTINGS.focusReadingEnabled = !SETTINGS.focusReadingEnabled;
      break;
    case StyleRow::GuideDots:
      SETTINGS.guideReadingEnabled = !SETTINGS.guideReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      SETTINGS.hyphenationEnabled = !SETTINGS.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      SETTINGS.embeddedStyle = !SETTINGS.embeddedStyle;
      break;
    case StyleRow::AntiAliasing:
      SETTINGS.textAntiAliasing = !SETTINGS.textAntiAliasing;
      break;

    default:
      return;
  }
  requestUpdate();
}

std::string TextSettingsActivity::styleValueText(int row) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::BionicReading:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::GuideDots:
      return SETTINGS.guideReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::Hyphenation:
      return SETTINGS.hyphenationEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::EmbeddedStyle:
      return SETTINGS.embeddedStyle ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::AntiAliasing:
      return SETTINGS.textAntiAliasing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);

    default:
      return "";
  }
}

// Embedded Style / AA do not change the sample pane; Bionic, Guide Dots, Hyphenation do.
bool TextSettingsActivity::focusedRowHasNoPreview() const {
  if (selectedIndex() == 0 || tab_ != Tab::Style) return false;
  const StyleRow row = static_cast<StyleRow>(selectedIndex() - 1);
  return row == StyleRow::EmbeddedStyle || row == StyleRow::AntiAliasing;
}

void TextSettingsActivity::switchTab(int direction, bool focusTabBar) {
  constexpr int tabCount = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + tabCount) % tabCount);
  // Per-tab nav cursor: stay on tab bar for side flips, else enter first list row.
  if (focusTabBar || currentListSize() <= 0) {
    selectedIndex() = 0;
  } else {
    selectedIndex() = 1;
  }
  requestUpdate();
}

int TextSettingsActivity::currentListSize() const {
  switch (tab_) {
    case Tab::Family:
      return static_cast<int>(fonts_.size());
    case Tab::Size:
      return static_cast<int>(sizes_.size());
    case Tab::Layout:
      return static_cast<int>(LayoutRow::Count);
    case Tab::Style:
      return static_cast<int>(StyleRow::Count);

    default:
      return 0;
  }
}

int& TextSettingsActivity::selectedIndex() { return selectedIndex_[static_cast<int>(tab_)]; }
int TextSettingsActivity::selectedIndex() const { return selectedIndex_[static_cast<int>(tab_)]; }
