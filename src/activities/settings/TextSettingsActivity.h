#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TextSettingsPreview.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Manage Fonts: header + tabs + list box (~half height) + Preview (~half).
// Layout is stable across tabs. Tab bar is nav ring position 0.
class TextSettingsActivity final : public Activity {
 public:
  // Download Fonts lives at the bottom of the Font list (not a fifth tab).
  enum class Tab : uint8_t { Family, Size, Layout, Style, Count };

  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       Tab initialTab = Tab::Family);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Row indices per tab. enum class (not plain enum) so a LayoutRow can't be
  // silently confused with a StyleRow of equal value.
  enum class LayoutRow { LineSpacing, ParaSpacing, Alignment, ScreenMargin, Count };
  // CrossInk order: Bionic Reading, Guide Dots, then layout/style toggles.
  enum class StyleRow { BionicReading, GuideDots, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  void applyFamily(int listIndex);
  void applySize(int listIndex);
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  // Applies the row at the given list index for the active tab (Confirm and tap share this).
  void activateRow(int row);

  // Handles tab/list/swipe touch input; returns true if an event was consumed (caller returns).
  bool handleTouch();

  // Fixed panes: tabs, settings box sized for Style rows, preview fills the rest.
  // Shared by render() and loop() so hit-testing matches drawing.
  struct PaneGeometry {
    int previewTop;
    int previewHeight;
    int tabTop;
    int listTop;
    int listHeight;
  };
  PaneGeometry paneGeometry() const;
  std::string layoutValueText(int row) const;
  std::string styleValueText(int row) const;
  // True when the focused list row is a setting the preview cannot reflect.
  bool focusedRowHasNoPreview() const;
  // direction: +1 / -1. focusTabBar: keep selection on the tab row (side flips);
  // false lands on the first list row (Confirm / open new tab).
  void switchTab(int direction = 1, bool focusTabBar = false);
  int currentListSize() const;
  // Navigation ring position for the active tab: 0 = tab bar, 1..N = list item N-1.
  int& selectedIndex();
  int selectedIndex() const;

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
    bool isDownloadAction = false;  // last row: open FontDownloadActivity
  };

  void rebuildFontList();
  // Drain the Confirm/Back/nav edge that opened this screen (same pattern as SettingsActivity).
  // Without this, Confirm-release cycles Font → Size on the first loop because selection
  // starts on the tab bar (index 0).
  void armAwaitOpenButtonRelease(bool force = false);

  struct SizeEntry {
    std::string name;
    uint8_t settingIndex;
  };

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;
  textsettings::PreviewLayout previewLayout_;  // cached preview line layout; relaid only on setting/geometry change

  Tab tab_;
  // per-Tab nav position (0 = tab bar, 1..N = row); onEnter starts at 0 for all tabs
  int selectedIndex_[static_cast<int>(Tab::Count)] = {};
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;
  bool awaitOpenButtonRelease_ = false;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
};
