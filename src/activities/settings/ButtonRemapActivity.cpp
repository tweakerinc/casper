#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/UiGhostPolicy.h"

namespace {
// Function options shown in the picker (order = display order).
constexpr uint8_t kFunctionChoices[] = {
    CrossPointSettings::BTN_FUNC_BACK,  CrossPointSettings::BTN_FUNC_CONFIRM, CrossPointSettings::BTN_FUNC_LEFT,
    CrossPointSettings::BTN_FUNC_RIGHT, CrossPointSettings::BTN_FUNC_UP,      CrossPointSettings::BTN_FUNC_DOWN,
    CrossPointSettings::BTN_FUNC_NONE,
};
constexpr int kFunctionChoiceCount = static_cast<int>(sizeof(kFunctionChoices) / sizeof(kFunctionChoices[0]));

// Bare / Dashboard (Casper) footers use MinimalTheme::drawButtonHints:
// four equal columns, UI_10, text centered (and truncated) in each slot.
// Do not use BaseTheme's fixed 80px pill X positions — outer slots were off.
constexpr int kFooterFontId = UI_10_FONT_ID;
constexpr int kFooterSlots = 4;

void drawDownArrow(const GfxRenderer& renderer, const int tipX, const int tipY, const int size) {
  // Tip at bottom (points down toward front keys); wide base above.
  for (int i = 0; i < size; ++i) {
    const int half = i;
    const int y = tipY - i;
    renderer.drawLine(tipX - half, y, tipX + half, y, true);
  }
}

void drawLeftArrow(const GfxRenderer& renderer, const int tipX, const int tipY, const int size) {
  // Tip at left (points left toward the side key); wide base on the right.
  for (int i = 0; i < size; ++i) {
    const int half = i;
    const int x = tipX + i;
    renderer.drawLine(x, tipY - half, x, tipY + half, true);
  }
}

void drawRightArrow(const GfxRenderer& renderer, const int tipX, const int tipY, const int size) {
  // Tip at right (points right toward the side key); wide base on the left.
  for (int i = 0; i < size; ++i) {
    const int half = i;
    const int x = tipX - i;
    renderer.drawLine(x, tipY - half, x, tipY + half, true);
  }
}
}  // namespace

void ButtonRemapActivity::onEnter() {
  Activity::onEnter();
  std::memcpy(tempMap, SETTINGS.hwButtonFunction, sizeof(tempMap));
  selectedIndex = 0;
  errorMessage.clear();
  errorUntil = 0;
  dirty = false;
  requestUpdate();
}

void ButtonRemapActivity::onExit() {
  // Persist only a valid map (auto-save when the user made valid edits).
  if (dirty && CrossPointSettings::isButtonFunctionMapValid(tempMap)) {
    SETTINGS.applyButtonFunctionMap(tempMap);
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

void ButtonRemapActivity::showError(const char* msg) {
  errorMessage = msg ? msg : "";
  errorUntil = millis() + 1800;
  requestUpdate();
}

void ButtonRemapActivity::resetDefaults() {
  CrossPointSettings::setDefaultButtonFunctionMap(tempMap);
  dirty = true;
  // Do not live-apply while this screen is open — footer chrome and nav stay locked.
  showError(tr(STR_REMAP_RESET_DONE));
}

bool ButtonRemapActivity::tryAssign(const uint8_t hwIndex, const uint8_t function) {
  if (hwIndex >= CrossPointSettings::HW_REMAP_BUTTON_COUNT) return false;
  if (function >= CrossPointSettings::BTN_FUNC_COUNT) return false;

  uint8_t trial[CrossPointSettings::HW_REMAP_BUTTON_COUNT];
  std::memcpy(trial, tempMap, sizeof(trial));

  // Already this function on this slot — no-op.
  if (trial[hwIndex] == function) {
    return true;
  }

  if (function == CrossPointSettings::BTN_FUNC_NONE) {
    // Disable this key only (NONE may appear on multiple slots).
    trial[hwIndex] = CrossPointSettings::BTN_FUNC_NONE;
  } else {
    // Each real function exists at most once: assigning it here swaps with the
    // slot that currently owns it (e.g. put Left on Bottom1 → Bottom1's old
    // function moves to wherever Left was). No puzzle, no duplicate Backs.
    int owner = -1;
    for (uint8_t i = 0; i < CrossPointSettings::HW_REMAP_BUTTON_COUNT; i++) {
      if (trial[i] == function) {
        owner = static_cast<int>(i);
        break;
      }
    }
    const uint8_t displaced = trial[hwIndex];
    trial[hwIndex] = function;
    if (owner >= 0) {
      trial[static_cast<uint8_t>(owner)] = displaced;
    }
  }

  if (!CrossPointSettings::isButtonFunctionMapValid(trial)) {
    showError(tr(STR_REMAP_NEED_CORE));
    return false;
  }
  std::memcpy(tempMap, trial, sizeof(tempMap));
  dirty = true;
  // Defer apply until exit so button hints / input stay stable while remapping.
  return true;
}

void ButtonRemapActivity::openFunctionPicker() {
  if (selectedIndex < 0 || selectedIndex >= kSlotCount) return;

  const uint8_t hw = static_cast<uint8_t>(selectedIndex);
  std::vector<std::string> labels;
  labels.reserve(kFunctionChoiceCount);
  int current = 0;
  for (int i = 0; i < kFunctionChoiceCount; i++) {
    labels.emplace_back(functionName(kFunctionChoices[i]));
    if (kFunctionChoices[i] == tempMap[hw]) current = i;
  }

  functionPopup.show(slotName(hw), labels, current, [this, hw](int idx) {
    if (idx < 0 || idx >= kFunctionChoiceCount) return;
    if (tryAssign(hw, kFunctionChoices[idx])) {
      requestUpdate();
    } else {
      requestUpdate();
    }
  });
  requestUpdate();
}

void ButtonRemapActivity::commitAndExit() {
  if (!CrossPointSettings::isButtonFunctionMapValid(tempMap)) {
    showError(tr(STR_REMAP_NEED_CORE));
    return;
  }
  dirty = true;
  SETTINGS.applyButtonFunctionMap(tempMap);
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("REMAP", "Failed to persist hwButtonFunction map");
    showError(tr(STR_REMAP_NEED_CORE));  // reuse; save failure is rare
    return;
  }
  dirty = false;  // already saved
  finish();
}

bool ButtonRemapActivity::lockedContinuousNav(const uint8_t hwIndex) {
  // Hold physical Up/Down slots to repeat list nav — independent of remap.
  if (!gpio.isPressed(hwIndex)) return false;
  const unsigned long held = mappedInput.getHeldTime();
  if (held < kLockedNavStartMs) return false;
  if (lastContinuousNavTime != 0 && (millis() - lastContinuousNavTime) < kLockedNavIntervalMs) {
    return false;
  }
  lastContinuousNavTime = millis();
  return true;
}

void ButtonRemapActivity::loop() {
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
  }

  // Popup + list: locked physical chrome only. Labels draw Back · Select · Up · Down
  // on front slots 0–3; input must match that layout so a remapped Back on Bottom 3
  // cannot steal "Back" while the footer still points at Bottom 1.
  if (functionPopup.handleInputLockedFront(mappedInput, [this] { requestUpdate(); })) {
    return;
  }

  // hw2 labeled Up, hw3 labeled Down (hardware names are LEFT/RIGHT).
  const bool prev = gpio.wasPressed(HalGPIO::BTN_LEFT) || lockedContinuousNav(HalGPIO::BTN_LEFT);
  const bool next = gpio.wasPressed(HalGPIO::BTN_RIGHT) || lockedContinuousNav(HalGPIO::BTN_RIGHT);
  if (gpio.wasReleased(HalGPIO::BTN_LEFT) || gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    lastContinuousNavTime = 0;
  }
  if (prev) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, kListCount);
    requestUpdate();
  } else if (next) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, kListCount);
    requestUpdate();
  }

  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    if (selectedIndex == kResetRow) {
      resetDefaults();
    } else {
      openFunctionPicker();
    }
    return;
  }

  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    commitAndExit();
    return;
  }
}

void ButtonRemapActivity::drawSlotArrows() const {
  if (selectedIndex < 0 || selectedIndex >= kSlotCount) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // Compact so side arrows sit cleanly in the left/right margin (no side labels).
  constexpr int kArrowSize = 7;
  const bool isX3 = gpio.deviceIsX3();

  if (selectedIndex < 4) {
    // Center on the locked footer labels (Back / Select / Up / Down), not live map text.
    const char* locked[] = {tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN)};
    const char* raw = locked[selectedIndex];
    const int slotW = pageWidth / kFooterSlots;
    int tipX = selectedIndex * slotW + slotW / 2;
    if (raw != nullptr && raw[0] != '\0') {
      const int maxLabelW = std::max(8, slotW - 8);
      const std::string label =
          renderer.truncatedText(kFooterFontId, raw, maxLabelW, EpdFontFamily::REGULAR);
      const int tw = renderer.getTextWidth(kFooterFontId, label.c_str(), EpdFontFamily::REGULAR);
      const int tx = selectedIndex * slotW + (slotW - tw) / 2;
      tipX = tx + tw / 2;
    }
    const int tipY = pageHeight - metrics.buttonHintsHeight - 2;
    drawDownArrow(renderer, tipX, tipY, kArrowSize);
    return;
  }

  // Side keys only — no side labels; arrows sit in the left/right margins.
  // Vertical positions match BaseTheme::drawSideButtonHints key chrome.
  constexpr int kSideBtnH = 80;
  constexpr int kSideMargin = 4;

  if (isX3) {
    constexpr int kX3SideY = 155;
    const int tipY = kX3SideY + kSideBtnH / 2;
    if (selectedIndex == 4) {
      // Left margin — tip near the left edge, pointing left at the key.
      drawLeftArrow(renderer, kSideMargin + 2, tipY, kArrowSize);
    } else {
      // Right margin — tip near the right edge, pointing right at the key.
      drawRightArrow(renderer, pageWidth - kSideMargin - 2, tipY, kArrowSize);
    }
  } else {
    // X4: both side keys on the right; arrows stay in the right margin.
    constexpr int kX4TopY = 345;
    const int tipX = pageWidth - kSideMargin - 2;
    if (selectedIndex == 4) {
      drawRightArrow(renderer, tipX, kX4TopY + kSideBtnH / 2, kArrowSize);
    } else {
      drawRightArrow(renderer, tipX, kX4TopY + kSideBtnH + kSideBtnH / 2, kArrowSize);
    }
  }
}

void ButtonRemapActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = UITheme::getInstance().getTheme();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Always full clear — never paint new footer text over old glyphs (popup used to
  // redraw mapLabels on top of function names without clearing).
  renderer.clearScreen();

  // Title + bottom rule (same as Settings / Manage Fonts header).
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REMAP_FRONT_BUTTONS));

  // Double-line hint band under the header — match Manage Fonts "Preview" chrome:
  // header already drew the top rule; label band; matching thin bottom rule.
  // Use the largest font that fits the full sentence (no ellipsis when possible).
  constexpr int kRuleThickness = 1;
  constexpr int kBandExtraPad = 10;
  const char* hint = tr(STR_REMAP_SLOT_HINT);
  const int maxHintW = std::max(40, pageWidth - metrics.contentSidePadding * 2);
  int hintFont = UI_12_FONT_ID;
  if (renderer.getTextWidth(UI_12_FONT_ID, hint, EpdFontFamily::REGULAR) > maxHintW) {
    hintFont = UI_10_FONT_ID;
    if (renderer.getTextWidth(UI_10_FONT_ID, hint, EpdFontFamily::REGULAR) > maxHintW) {
      hintFont = SMALL_FONT_ID;
    }
  }
  const int chromeLineH = renderer.getLineHeight(hintFont);
  const int labelBandH = chromeLineH + kBandExtraPad;
  const int labelBandTop = metrics.topPadding + metrics.headerHeight;  // text area below header rule
  const int labelY = labelBandTop + (labelBandH - chromeLineH) / 2;
  // Only truncate if even SMALL_FONT cannot fit (long translations).
  const std::string hintShown =
      renderer.truncatedText(hintFont, hint, maxHintW, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(hintFont, labelY, hintShown.c_str(), true, EpdFontFamily::REGULAR);
  const int bottomRuleY = labelBandTop + labelBandH;
  renderer.drawLine(0, bottomRuleY, pageWidth - 1, bottomRuleY, kRuleThickness, true);

  // Vertically center the list in the free band above the button hints so arrows
  // sit closer to the front keys with less empty dead space below.
  const int rowStep = std::max(1, theme.getListRowStep(false));
  const int listContentH = rowStep * kListCount;
  const int availTop = bottomRuleY + kRuleThickness + metrics.verticalSpacing;
  const int availBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int availH = std::max(0, availBottom - availTop);
  const int listH = std::min(listContentH, availH);
  const int listTop = availTop + std::max(0, (availH - listH) / 2);

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, kListCount, selectedIndex,
      [this](int index) -> std::string {
        if (index == kResetRow) return tr(STR_REMAP_RESET_DEFAULTS);
        return slotName(static_cast<uint8_t>(index));
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (index == kResetRow) return "";
        return functionName(tempMap[static_cast<uint8_t>(index)]);
      },
      true);

  if (!errorMessage.empty() && !functionPopup.isActive()) {
    GUI.drawHelpText(renderer,
                     Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 18, pageWidth, 18},
                     errorMessage.c_str());
  }

  // Locked chrome: physical Back · Select · Up · Down (hw 0–3). loop() reads the
  // same raw keys — never MappedInputManager — so the user's remap cannot desync
  // footer labels from what the buttons actually do on this screen.
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  // Popup draws only the dialog (no second footer pass). Full clear above prevents overdraw.
  if (functionPopup.isActive()) {
    functionPopup.render(renderer);
  } else {
    drawSlotArrows();
  }

  UiGhostPolicy::displayMenuFrame(renderer);
}

const char* ButtonRemapActivity::slotName(const uint8_t hwIndex) const {
  switch (hwIndex) {
    case 0:
      return tr(STR_REMAP_SLOT_FRONT_1);
    case 1:
      return tr(STR_REMAP_SLOT_FRONT_2);
    case 2:
      return tr(STR_REMAP_SLOT_FRONT_3);
    case 3:
      return tr(STR_REMAP_SLOT_FRONT_4);
    case 4:
      // X3: side keys are left/right. X4: same hw slots are upper/lower (Up/Down feel).
      return gpio.deviceIsX3() ? tr(STR_REMAP_SLOT_SIDE_LEFT) : tr(STR_REMAP_SLOT_SIDE_UPPER);
    case 5:
      return gpio.deviceIsX3() ? tr(STR_REMAP_SLOT_SIDE_RIGHT) : tr(STR_REMAP_SLOT_SIDE_LOWER);
    default:
      return "";
  }
}

const char* ButtonRemapActivity::functionName(const uint8_t function) const {
  switch (function) {
    case CrossPointSettings::BTN_FUNC_BACK:
      return tr(STR_BACK);
    case CrossPointSettings::BTN_FUNC_CONFIRM:
      // Menus label this action "Select"; keep the remapper consistent.
      return tr(STR_SELECT);
    case CrossPointSettings::BTN_FUNC_LEFT:
      return tr(STR_DIR_LEFT);
    case CrossPointSettings::BTN_FUNC_RIGHT:
      return tr(STR_DIR_RIGHT);
    case CrossPointSettings::BTN_FUNC_UP:
      return tr(STR_DIR_UP);
    case CrossPointSettings::BTN_FUNC_DOWN:
      return tr(STR_DIR_DOWN);
    case CrossPointSettings::BTN_FUNC_NONE:
      return tr(STR_DISABLED);
    default:
      return "";
  }
}
