#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::formatValueText(char* buf, const size_t len, const int v) const {
  if (!buf || len == 0) {
    return;
  }
  if (minBoundaryLabelId != StrId::STR_NONE_OPT && v == minValue) {
    snprintf(buf, len, "%s", I18N.get(minBoundaryLabelId));
    return;
  }
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && v == maxValue) {
    snprintf(buf, len, "%s", I18N.get(maxBoundaryLabelId));
    return;
  }
  if (displayStyle == DisplayStyle::HoursMinutes) {
    // Value is total minutes → "1H 5M" / "0H 30M" (no mental math).
    const unsigned h = static_cast<unsigned>(v) / 60u;
    const unsigned m = static_cast<unsigned>(v) % 60u;
    snprintf(buf, len, "%uH %uM", h, m);
    return;
  }
  if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(buf, len, I18N.get(valueFormatId), static_cast<unsigned int>(v));
    return;
  }
  snprintf(buf, len, "%d", v);
}

void IntervalSelectionActivity::drawStepHintLine(const int y, const char* dirA, const char* dirB, const int step) {
  char stepText[24];
  if (displayStyle == DisplayStyle::HoursMinutes) {
    if (step >= 60 && step % 60 == 0) {
      snprintf(stepText, sizeof(stepText), "%uH", static_cast<unsigned>(step / 60));
    } else if (step >= 60) {
      snprintf(stepText, sizeof(stepText), "%uH %uM", static_cast<unsigned>(step / 60),
               static_cast<unsigned>(step % 60));
    } else {
      snprintf(stepText, sizeof(stepText), "%uM", static_cast<unsigned>(step));
    }
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(stepText, sizeof(stepText), I18N.get(valueFormatId), static_cast<unsigned int>(step));
  } else {
    snprintf(stepText, sizeof(stepText), "%d", step);
  }
  char line[80];
  // "Up / Down: 1%" style — direction names come from i18n so remaps stay clear.
  snprintf(line, sizeof(line), "%s / %s: %s", dirA, dirB, stepText);
  renderer.drawCenteredText(SMALL_FONT_ID, y, line, true);
}

void IntervalSelectionActivity::loop() {
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  int tx = 0;
  int ty = 0;
  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  // Live drag on the slider: once a touch lands on the bar, the value follows the
  // finger until release. Runs before the Back/Confirm handlers because the release
  // of a drag can also register as a swipe (e.g. the left-edge rightward back
  // gesture) — the drag must consume it so it can't cancel or confirm the dialog.
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    if (draggingBar || (ty >= barY - 20 && ty < barY + barHeight + 20 && tx >= barX && tx < barX + barWidth)) {
      draggingBar = true;
      const int range = std::max(1, maxValue - minValue);
      const int dragged =
          clampedValue(minValue + std::clamp(tx - barX, 0, barWidth - 1) * range / std::max(1, barWidth - 1));
      if (dragged != value) {
        value = dragged;
        requestUpdate();
      }
      return;
    }
  } else if (draggingBar) {
    // Release frame of a drag: swallow the tap/swipe events it produced.
    draggingBar = false;
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
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (ty >= barY - 20 && ty < barY + barHeight + 20 && tx >= barX && tx < barX + barWidth) {
      const int range = std::max(1, maxValue - minValue);
      value = clampedValue(minValue + (tx - barX) * range / std::max(1, barWidth - 1));
      requestUpdate();
      return;
    }
    if (ty >= renderer.getScreenHeight() - 80) {
      if (tx < renderer.getScreenWidth() / 3) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
      } else if (tx > renderer.getScreenWidth() * 2 / 3) {
        setResult(IntervalResult{static_cast<uint32_t>(value)});
        finish();
      }
      return;
    }
  }

  // Match the rest of the app: logical Up/Down = fine step, Left/Right = coarse.
  // Uses MappedInputManager so remapped buttons follow the user's layout.
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-largeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(largeStep); });
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(titleId), true, EpdFontFamily::BOLD);

  char formattedValue[32];
  formatValueText(formattedValue, sizeof(formattedValue), value);
  renderer.drawCenteredText(UI_12_FONT_ID, 90, formattedValue, true, EpdFontFamily::BOLD);

  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = (barWidth - 4) * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Logical axes (same scheme as lists/menus): Up/Down fine, Left/Right coarse.
  drawStepHintLine(barY + 30, tr(STR_DIR_UP), tr(STR_DIR_DOWN), smallStep);
  drawStepHintLine(barY + 52, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), largeStep);

  // Footer: Back/Select; −/+ track the fine Up/Down step.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
