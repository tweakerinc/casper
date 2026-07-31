#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <components/bars/tap-zones.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

// Long-hold Back to leave reader / go home (half of original 1s).
constexpr unsigned long GO_HOME_MS = 500;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

// Extra air under top chrome (battery/clock) and matching reserve above bottom
// chrome so page text never sits under the status bar or dictionary button strip.
constexpr int kReaderTopChromeExtra = 24;
constexpr int kReaderBottomChromeExtra = 24;  // mirror top air
constexpr int kReaderBottomChromePad = 4;

enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool prev =
      tiltPrev ||
      (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton))
                : (input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton)));
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const unsigned long held = input.getHeldTime();
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              held < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             held >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasPressed(nextButton))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasReleased(nextButton)));
  return {prev, next, tiltPrev || tiltNext};
}

// True while any physical/logical control that can emit a page-turn edge is held.
// Used to swallow bounce after a turn until the gesture fully ends.
inline bool anyPageTurnControlHeld(const MappedInputManager& input) {
  return input.isPressed(MappedInputManager::Button::PageBack) ||
         input.isPressed(MappedInputManager::Button::PageForward) ||
         input.isPressed(MappedInputManager::Button::Left) ||
         input.isPressed(MappedInputManager::Button::Right) ||
         input.isPressed(MappedInputManager::Button::Up) ||
         input.isPressed(MappedInputManager::Button::Down) ||
         input.isPressed(MappedInputManager::Button::Power);
}

// One intentional gesture → one page turn. Contact bounce / ADC chatter on
// Xteink ladders can emit several wasPressed/wasReleased edges per press; without
// a latch the reader advances once per edge before e-ink paints.
//
// Policy:
//  - Accept a turn edge only when not waiting for release and min interval elapsed.
//  - After accept, ignore further edges until all page-turn controls are released.
//  - Pure tilt events are rate-limited only (sensor has its own re-arm).
struct PageTurnLatch {
  bool waitingRelease = false;
  unsigned long lastAcceptedMs = 0;
  // Long enough to cover typical membrane bounce; short enough for deliberate rapid turns.
  static constexpr unsigned long kMinIntervalMs = 180;

  // Call every loop when there is no turn edge so we re-arm after release.
  void pollIdle(const MappedInputManager& input) {
    if (waitingRelease && !anyPageTurnControlHeld(input)) {
      waitingRelease = false;
    }
  }

  // Returns true if prev/next should be acted on. Clears both when rejected.
  bool accept(bool& prev, bool& next, const bool fromTilt, const bool fromTouch, const MappedInputManager& input) {
    if (!prev && !next) {
      pollIdle(input);
      return false;
    }

    const unsigned long now = millis();

    // Pure tilt: sensor already re-arms; only rate-limit.
    if (fromTilt && !fromTouch && !anyPageTurnControlHeld(input)) {
      if (now - lastAcceptedMs < kMinIntervalMs) {
        prev = false;
        next = false;
        return false;
      }
      lastAcceptedMs = now;
      return true;
    }

    if (waitingRelease) {
      if (!anyPageTurnControlHeld(input)) {
        waitingRelease = false;
      }
      // Never accept the same-frame residual edge that coincided with release.
      prev = false;
      next = false;
      return false;
    }

    if (now - lastAcceptedMs < kMinIntervalMs) {
      prev = false;
      next = false;
      return false;
    }

    waitingRelease = true;
    lastAcceptedMs = now;
    return true;
  }
};

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(GfxRenderer& renderer, const MappedInputManager& input) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  const int16_t width = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight());
  const int16_t previousZoneWidth = width / 3;
  const freeink::ui::TapZone zones[] = {
      {freeink::ui::Rect{0, 0, previousZoneWidth, height}, READER_TOUCH_PREV},
      {freeink::ui::Rect{previousZoneWidth, 0, static_cast<int16_t>(width - previousZoneWidth), height},
       READER_TOUCH_NEXT},
  };

  for (const auto& zone : zones) {
    if (!zone.enabled || !zone.rect.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) continue;
    result.prev = zone.action == READER_TOUCH_PREV;
    result.next = zone.action == READER_TOUCH_NEXT;
    break;
  }
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// Reader menu opens on a downward swipe from the top edge (replaces the old center tap-and-hold).
inline bool isTouchMenuGesture(const MappedInputManager& input) {
  return SETTINGS.touchReaderControls && input.hasTouch() && input.wasMenuGesture();
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  if (async) {
    renderer.displayBufferAsync(mode);
  } else {
    renderer.displayBuffer(mode);
  }
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
//
// Long press (observed while still held for >= GO_BACK_OR_HOME_MS):
// - default: go to file browser
// - with backShortToFileBrowser: go home
// Short press (Back release that was not already handled as long-press):
// - default: go home
// - with backShortToFileBrowser: go to file browser
//
// Important: any Back *release* always navigates short. We must not require
// held < threshold on release. When the main loop is busy (section build, SD,
// first-page work), a physical short press can be sampled only on release with
// held already >= threshold because isPressed was never polled during the hold.
// The old (wasReleased && held < 500) check then no-oped — feeling like
// "Back needs two presses". Long-press is only the while-held branch so a
// missed long sample still leaves via short (home), which matches intent.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome) {
  // Long-press only while held — must be observed across the threshold in-loop.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_BACK_OR_HOME_MS) {
    if (SETTINGS.backShortToFileBrowser) {
      goHome.fn(goHome.ctx);
    } else {
      activityManager.goToFileBrowser(filePath);
    }
    return true;
  }
  // Any Back release → short navigation. Do not gate on held time (see above).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (SETTINGS.backShortToFileBrowser) {
      activityManager.goToFileBrowser(filePath);
    } else {
      goHome.fn(goHome.ctx);
    }
    return true;
  }
  return false;
}

}  // namespace ReaderUtils
