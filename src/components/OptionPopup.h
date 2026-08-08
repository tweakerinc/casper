#pragma once
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    beginShow();
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    beginShow();
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    beginShow();
  }

  void show(const char* titleStr, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr ? titleStr : "";
    ownedStrings = options;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    beginShow();
  }

  // Normal menus: logical remapped buttons (Back/Confirm/Up/Down/Left/Right).
  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    return handleInputCommon(input, requestUpdate, /*lockedFrontChrome=*/false);
  }

  // Remap Buttons editor: physical front only — Back · Select · Up · Down on hw 0–3.
  // Ignores the user's remap so labels and presses always match the locked footer.
  bool handleInputLockedFront(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    return handleInputCommon(input, requestUpdate, /*lockedFrontChrome=*/true);
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex);
  }

  // Visible, or still owning open/dismiss click edges so the host does not act.
  bool isActive() const { return active || drainingDismiss_ || awaitOpenRelease_; }

 private:
  struct Layout {
    Rect dialog{0, 0, 0, 0};
    std::vector<Rect> options;
  };

  void beginShow() {
    layoutValid = false;
    drainingDismiss_ = false;
    // Hosts open this popup on Confirm *press* (Settings → Short Power) or
    // *release* (Manage Fonts). Always wait out the open gesture before any
    // select/dismiss so the same click cannot open-and-close in one motion.
    awaitOpenRelease_ = true;
    active = true;
  }

  // Wait until nav/confirm/back are up, then consume residual edges.
  // Used both after show() (open gesture) and after select/back (dismiss).
  bool drainButtonGesture(MappedInputManager& input, const bool lockedFrontChrome, bool& flag) {
    using B = MappedInputManager::Button;
    bool held = false;
    if (lockedFrontChrome) {
      held = gpio.isPressed(HalGPIO::BTN_CONFIRM) || gpio.isPressed(HalGPIO::BTN_BACK) ||
             gpio.isPressed(HalGPIO::BTN_LEFT) || gpio.isPressed(HalGPIO::BTN_RIGHT);
    } else {
      held = input.isPressed(B::Confirm) || input.isPressed(B::Back) || input.isPressed(B::Up) ||
             input.isPressed(B::Down) || input.isPressed(B::Left) || input.isPressed(B::Right) ||
             input.isFrontButtonPressed(HalGPIO::BTN_BACK) || input.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
             input.isFrontButtonPressed(HalGPIO::BTN_LEFT) || input.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
    }
    if (held) return true;

    if (lockedFrontChrome) {
      (void)gpio.wasPressed(HalGPIO::BTN_CONFIRM);
      (void)gpio.wasReleased(HalGPIO::BTN_CONFIRM);
      (void)gpio.wasPressed(HalGPIO::BTN_BACK);
      (void)gpio.wasReleased(HalGPIO::BTN_BACK);
      (void)gpio.wasPressed(HalGPIO::BTN_LEFT);
      (void)gpio.wasReleased(HalGPIO::BTN_LEFT);
      (void)gpio.wasPressed(HalGPIO::BTN_RIGHT);
      (void)gpio.wasReleased(HalGPIO::BTN_RIGHT);
    } else {
      (void)input.wasPressed(B::Confirm);
      (void)input.wasReleased(B::Confirm);
      (void)input.wasPressed(B::Back);
      (void)input.wasReleased(B::Back);
      (void)input.wasPressed(B::Up);
      (void)input.wasReleased(B::Up);
      (void)input.wasPressed(B::Down);
      (void)input.wasReleased(B::Down);
      (void)input.wasPressed(B::Left);
      (void)input.wasReleased(B::Left);
      (void)input.wasPressed(B::Right);
      (void)input.wasReleased(B::Right);
      (void)input.getReleasedFrontButton();
      (void)input.getPressedFrontButton();
    }
    flag = false;
    return true;
  }

  void beginDismissDrain() { drainingDismiss_ = true; }

  bool handleInputCommon(MappedInputManager& input, const std::function<void()>& requestUpdate,
                         const bool lockedFrontChrome) {
    if (drainingDismiss_) {
      return drainButtonGesture(input, lockedFrontChrome, drainingDismiss_);
    }
    if (awaitOpenRelease_) {
      // Popup is visible; ignore select until the open click fully finishes.
      (void)drainButtonGesture(input, lockedFrontChrome, awaitOpenRelease_);
      return true;
    }
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    int tx = 0;
    int ty = 0;
    if (input.wasScreenTouchDown(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          if (selectedIndex != i) {
            selectedIndex = i;
            layoutValid = false;
            requestUpdate();
          }
          break;
        }
      }
      return true;
    }
    if (input.wasScreenTapped(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          selectedIndex = i;
          active = false;
          if (onSelectCallback) onSelectCallback(selectedIndex);
          // Touch has no press/release split with keyboard — no button drain.
          requestUpdate();
          return true;
        }
      }
      // Taps on the dialog chrome (title, padding) keep the popup open; taps outside dismiss it
      if (contains(hitLayout.dialog, tx, ty)) return true;
      active = false;
      requestUpdate();
      return true;
    }

    // Locked editor chrome: raw front slots match fixed footer labels
    // (Back · Select · Up · Down) on hw 0–3 — not the user's remapped functions.
    //
    // Normal popups: same orientation-aware front Up/Down as EpubReaderMenuActivity
    // (getFrontPrevious/NextButtons). Raw Up/Down ignored Orient Front Buttons while
    // mapLabels still swapped the footer — Portrait 180° then moved opposite the labels.
    auto anyPressed = [&input](const std::vector<MappedInputManager::Button>& buttons) {
      for (const auto button : buttons) {
        if (input.wasPressed(button)) {
          return true;
        }
      }
      return false;
    };
    const bool prev = lockedFrontChrome ? gpio.wasPressed(HalGPIO::BTN_LEFT)
                                        : anyPressed(ButtonNavigator::getFrontPreviousButtons());
    const bool next = lockedFrontChrome ? gpio.wasPressed(HalGPIO::BTN_RIGHT)
                                        : anyPressed(ButtonNavigator::getFrontNextButtons());
    // Select / dismiss on *press* (classic OptionPopup). Open-gesture drain
    // above blocks the host's opening Confirm from also selecting here; dismiss
    // drain below blocks release-driven hosts from acting on the same click.
    const bool confirm =
        lockedFrontChrome ? gpio.wasPressed(HalGPIO::BTN_CONFIRM)
                          : input.wasPressed(MappedInputManager::Button::Confirm);
    const bool back =
        lockedFrontChrome ? gpio.wasPressed(HalGPIO::BTN_BACK) : input.wasPressed(MappedInputManager::Button::Back);

    if (prev) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      layoutValid = false;  // scroll window may move with selection
      requestUpdate();
      return true;
    }
    if (next) {
      selectedIndex = (selectedIndex + 1) % count;
      layoutValid = false;
      requestUpdate();
      return true;
    }
    if (confirm) {
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      beginDismissDrain();
      requestUpdate();
      return true;
    }
    if (back) {
      active = false;
      beginDismissDrain();
      requestUpdate();
      return true;
    }
    return true;
  }

  // Text measurement is expensive and wasScreenTouchDown() is level-triggered, so the
  // layout is computed once per show() and cached rather than rebuilt every loop().
  const Layout& getLayout(const GfxRenderer& renderer) const {
    if (layoutValid) return layout;

    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
    // Match BaseTheme::drawOptionPopup: focus is bold-only; size against BOLD.
    (void)metrics.optionPopupOptionFontBold;

    const int itemSpacing = metrics.optionPopupItemSpacing;
    const int innerPadding = metrics.optionPopupInnerPadding;
    const int selectionHPadding = metrics.optionPopupSelectionHPadding;
    const int selectionVPadding = metrics.optionPopupSelectionVPadding;

    const int optionLineHeight = renderer.getLineHeight(optionFontId);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int rowHeight = optionLineHeight + selectionVPadding * 2;
    const int rowStep = rowHeight + itemSpacing;

    int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    for (const auto& opt : ownedStrings) {
      const int width = renderer.getTextWidth(optionFontId, opt.c_str(), EpdFontFamily::BOLD);
      if (width > maxTextWidth) maxTextWidth = width;
    }

    const int optionCount = static_cast<int>(ownedStrings.size());
    const int topMargin = 8;
    const int bottomMargin = metrics.buttonHintsHeight + 8;
    const int maxDialogH = std::max(rowHeight + innerPadding * 2 + titleLineHeight + metrics.optionPopupTitleGap,
                                    pageHeight - topMargin - bottomMargin);
    const int chromeH = innerPadding * 2 + titleLineHeight + metrics.optionPopupTitleGap;
    const int maxListH = std::max(rowHeight, maxDialogH - chromeH);
    int visibleCount = optionCount;
    if (rowStep > 0) {
      visibleCount = std::max(1, (maxListH + itemSpacing) / rowStep);
    }
    if (visibleCount > optionCount) visibleCount = optionCount;

    int firstVisible = 0;
    if (optionCount > visibleCount) {
      firstVisible = selectedIndex - (visibleCount - 1) / 2;
      if (firstVisible < 0) firstVisible = 0;
      if (firstVisible > optionCount - visibleCount) firstVisible = optionCount - visibleCount;
    }

    const int listHeight = visibleCount > 0
                               ? rowHeight * visibleCount + itemSpacing * std::max(0, visibleCount - 1)
                               : 0;
    const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2) * 12 / 10,
                                 pageWidth - metrics.optionPopupDialogSideMargin * 2);
    const int contentHeight = titleLineHeight + metrics.optionPopupTitleGap + listHeight;
    const int dialogH = std::min(contentHeight + innerPadding * 2, maxDialogH);
    const int dialogX = (pageWidth - dialogW) / 2;
    const int availH = pageHeight - topMargin - bottomMargin;
    const int dialogY = topMargin + std::max(0, (availH - dialogH) / 2);
    const bool showScroll = optionCount > visibleCount;
    constexpr int kScrollReserve = 10;
    const int itemRectX = dialogX + innerPadding;
    const int itemRectW = dialogW - innerPadding * 2 - (showScroll ? kScrollReserve : 0);
    const int firstItemY = dialogY + innerPadding + titleLineHeight + metrics.optionPopupTitleGap;

    layout.dialog = Rect{dialogX, dialogY, dialogW, dialogH};
    layout.options.clear();
    layout.options.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      if (i >= firstVisible && i < firstVisible + visibleCount) {
        const int vis = i - firstVisible;
        layout.options[i] = Rect{itemRectX, firstItemY + vis * rowStep, itemRectW, rowHeight};
      } else {
        layout.options[i] = Rect{0, 0, 0, 0};
      }
    }
    layoutValid = true;
    return layout;
  }

  static bool contains(const Rect& rect, const int x, const int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
  }

  bool active = false;
  // True after show() until the open Confirm/Back fully releases (Settings press-open).
  bool awaitOpenRelease_ = false;
  // True after Confirm/Back dismiss until residual release edges are consumed.
  bool drainingDismiss_ = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  mutable Layout layout;
  mutable bool layoutValid = false;
};
