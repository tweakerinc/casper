#include "MappedInputManager.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. Home/settings force Portrait, so they never swap.
  // When Orient Front Buttons is On:
  //   Portrait 180° — full axis follow (working as intended).
  //   Landscape CCW — front slot 3 (Up func) acts/labels as Down; slot 4 as Up.
  // Portrait and Landscape CW never swap here (Orient defaults Off for those layouts;
  // CW+On would feel inverted for page turn, so leave mapping alone).
  if (!SETTINGS.frontButtonFollowOrientation) {
    return false;
  }
  const auto o = renderer.getOrientation();
  return o == GfxRenderer::PortraitInverted || o == GfxRenderer::LandscapeCounterClockwise;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;
  const bool orientSwap = isNavDirectionSwapped();

  // Any physical key (front or side) assigned this function.
  auto anyWithFunc = [&](const uint8_t func) -> bool {
    for (uint8_t hw = 0; hw < CrossPointSettings::HW_REMAP_BUTTON_COUNT; hw++) {
      if (SETTINGS.hwButtonFunction[hw] == func && (gpio.*fn)(hw)) return true;
    }
    return false;
  };

  switch (button) {
    case Button::Back:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_BACK);
    case Button::Confirm:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_CONFIRM);
    case Button::Left:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_LEFT);
    case Button::Right:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_RIGHT);
    case Button::Up:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_UP);
    case Button::Down:
      return anyWithFunc(CrossPointSettings::BTN_FUNC_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack: {
      // Page turn uses Up/Down + Left/Right. Side layout swaps axes; orientSwap reverses
      // both so Portrait 180 / landscape keep "forward" feeling correct. (Previously only
      // Left/Right were flipped in detectPageTurn — Up/Down never followed orientation.)
      const bool prevIsUpLeft = (sideLayout == CrossPointSettings::PREV_NEXT) != orientSwap;
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
        case CrossPointSettings::NEXT_PREV:
          if (prevIsUpLeft) {
            return anyWithFunc(CrossPointSettings::BTN_FUNC_UP) || anyWithFunc(CrossPointSettings::BTN_FUNC_LEFT);
          }
          return anyWithFunc(CrossPointSettings::BTN_FUNC_DOWN) || anyWithFunc(CrossPointSettings::BTN_FUNC_RIGHT);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          // Still allow remapped front Up/Down/Left/Right for page turn when "sides disabled"
          // only meant the side-layout enum; keep prior behavior (no page from layout).
          return false;
      }
    }
    case Button::PageForward: {
      const bool nextIsDownRight = (sideLayout == CrossPointSettings::PREV_NEXT) != orientSwap;
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
        case CrossPointSettings::NEXT_PREV:
          if (nextIsDownRight) {
            return anyWithFunc(CrossPointSettings::BTN_FUNC_DOWN) || anyWithFunc(CrossPointSettings::BTN_FUNC_RIGHT);
          }
          return anyWithFunc(CrossPointSettings::BTN_FUNC_UP) || anyWithFunc(CrossPointSettings::BTN_FUNC_LEFT);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    }
    case Button::NavNext:
      // Logical "next item": Down+Right, flipped with orientation follow.
      return orientSwap ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                        : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      return orientSwap ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                        : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
  }

  return false;
}

namespace {
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int bottomEdgeTop =
        renderer.getScreenHeight() - static_cast<int>(renderer.getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Orientation swap flips each pair so physical feel matches a rotated reader.
  const bool swapLabels = isNavDirectionSwapped();
  // Vertical pair: callers usually pass Up/Down as previous/next for list menus.
  const char* upLabel = swapLabels ? next : previous;
  const char* downLabel = swapLabels ? previous : next;
  // Horizontal pair: always show true Left/Right names (not Up/Down aliases).
  // Previously Left/Right reused previous/next, so remapped "Left" still read "Up".
  const char* leftLabel = swapLabels ? tr(STR_DIR_RIGHT) : tr(STR_DIR_LEFT);
  const char* rightLabel = swapLabels ? tr(STR_DIR_LEFT) : tr(STR_DIR_RIGHT);

  // Label each physical front slot by the function assigned to it.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    if (hw >= CrossPointSettings::HW_REMAP_BUTTON_COUNT) return "";
    switch (SETTINGS.hwButtonFunction[hw]) {
      case CrossPointSettings::BTN_FUNC_BACK:
        return back;
      case CrossPointSettings::BTN_FUNC_CONFIRM:
        return confirm;
      case CrossPointSettings::BTN_FUNC_LEFT:
        return leftLabel;
      case CrossPointSettings::BTN_FUNC_RIGHT:
        return rightLabel;
      case CrossPointSettings::BTN_FUNC_UP:
        return upLabel;
      case CrossPointSettings::BTN_FUNC_DOWN:
        return downLabel;
      default:
        return "";
    }
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

namespace {
const char* directionFuncCaption(const uint8_t func, const char* backAction, const char* confirmAction) {
  using F = CrossPointSettings::BUTTON_FUNCTION;
  switch (func) {
    case F::BTN_FUNC_BACK:
      return backAction ? backAction : tr(STR_BACK);
    case F::BTN_FUNC_CONFIRM:
      return confirmAction ? confirmAction : tr(STR_SELECT);
    case F::BTN_FUNC_LEFT:
      return tr(STR_DIR_LEFT);
    case F::BTN_FUNC_RIGHT:
      return tr(STR_DIR_RIGHT);
    case F::BTN_FUNC_UP:
      return tr(STR_DIR_UP);
    case F::BTN_FUNC_DOWN:
      return tr(STR_DIR_DOWN);
    default:
      return "";
  }
}
}  // namespace

MappedInputManager::Labels MappedInputManager::mapDirectionLabels(const char* backAction,
                                                                  const char* confirmAction) const {
  // Physical front L→R (hw 0–3): caption matches remapped function exactly.
  const auto& map = SETTINGS.hwButtonFunction;
  return {directionFuncCaption(map[CrossPointSettings::FRONT_HW_BACK], backAction, confirmAction),
          directionFuncCaption(map[CrossPointSettings::FRONT_HW_CONFIRM], backAction, confirmAction),
          directionFuncCaption(map[CrossPointSettings::FRONT_HW_LEFT], backAction, confirmAction),
          directionFuncCaption(map[CrossPointSettings::FRONT_HW_RIGHT], backAction, confirmAction)};
}

void MappedInputManager::mapSideDirectionLabels(const char*& sideA, const char*& sideB) const {
  // hw 4 = X3 left / X4 upper; hw 5 = X3 right / X4 lower.
  const auto& map = SETTINGS.hwButtonFunction;
  sideA = directionFuncCaption(map[4], tr(STR_BACK), tr(STR_SELECT));
  sideB = directionFuncCaption(map[5], tr(STR_BACK), tr(STR_SELECT));
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

int MappedInputManager::getReleasedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // Bypasses remapping for screens whose labels are fixed to physical slots.
  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

bool MappedInputManager::isFrontButtonPressed(const uint8_t buttonIndex) const { return gpio.isPressed(buttonIndex); }
