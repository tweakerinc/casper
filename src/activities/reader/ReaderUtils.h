#pragma once

#include <CasperSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <components/bars/tap-zones.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

// Dark Mode (Settings → Display → Dark Mode / reader menu). Master default Off.
inline bool darkModeEnabled() { return SETTINGS.readerDarkMode != 0; }

// Nested "Reader Only": Off = whole UI (GfxRenderer invert-on-display); On = book pages only.
inline bool darkModeReaderOnly() { return SETTINGS.darkModeReaderOnly != 0; }

// True when home/menus/reader all invert at display time.
inline bool systemWideDarkMode() { return darkModeEnabled() && !darkModeReaderOnly(); }

// True when the book page should appear dark (either scope).
inline bool readerDarkModeEnabled() { return darkModeEnabled(); }

// True when the book page should invert for display without arming whole-UI invert.
// FB stays in light paint-space; invert only around the panel push (see display*).
inline bool readerOnlyDarkPaint() { return darkModeEnabled() && darkModeReaderOnly(); }

// DEPRECATED path that permanently inverted the FB (sleep/wake and home leaked
// polarity). Prefer displayWithRefreshCycle / displayWithDarkMode which invert
// only for the panel push. No-op kept so call sites compile while migrating.
inline void applyReaderDarkModeIfEnabled(const GfxRenderer& /*renderer*/) {}

// Push the current framebuffer to the panel with Dark Mode polarity.
// - System-wide: invertOnDisplay is armed by loop(); displayBuffer inverts there.
// - Reader-only: temporary invert around the refresh so home/menus stay light.
// Use for reader pages and reader-context chrome (not home).
inline void displayWithDarkMode(const GfxRenderer& renderer,
                                const HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH) {
  if (readerOnlyDarkPaint() && !renderer.getInvertOnDisplay()) {
    renderer.invertScreen();
    renderer.displayBuffer(mode);
    renderer.invertScreen();
    return;
  }
  renderer.displayBuffer(mode);
}

// Hold threshold used by end-of-book short-Back (last page) and similar chords.
// Reader leave-to-home is on Back *release* only (no long-press Back).
constexpr unsigned long GO_HOME_MS = 500;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;  // alias kept for call sites
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;
// Max gap between two Confirm releases to count as Double-Press Menu.
// Wait after Confirm release before opening the menu when double-press is enabled.
// Was 400ms; shave it so single-tap menu still feels snappy without killing double-tap.
constexpr unsigned long DOUBLE_PRESS_MENU_MS = 220;

// Extra air under top chrome (battery/clock) and matching reserve above bottom
// chrome so page text never sits under the status bar or dictionary button strip.
// Bumps slightly when Manage Reader UI → Font Size is larger than 8 pt.
inline int readerTopChromeExtra() {
  switch (SETTINGS.statusBarFontSize) {
    case CasperSettings::STATUS_BAR_FONT_10:
      return 28;
    case CasperSettings::STATUS_BAR_FONT_12:
      return 32;
    default:
      return 24;
  }
}
inline int readerBottomChromeExtra() { return readerTopChromeExtra(); }
constexpr int kReaderTopChromeExtra = 24;     // legacy default; prefer readerTopChromeExtra()
constexpr int kReaderBottomChromeExtra = 24;  // mirror top air
constexpr int kReaderBottomChromePad = 4;

enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CasperSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CasperSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CasperSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CasperSettings::ORIENTATION::LANDSCAPE_CCW:
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
  const bool usePress =
      SETTINGS.longPressSideA == SETTINGS.LP_MENU_DISABLED && SETTINGS.longPressSideB == SETTINGS.LP_MENU_DISABLED;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  // PageBack/PageForward already include Up/Down/Left/Right + Side Layout + Orient Front Buttons.
  const bool prev = tiltPrev || (usePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                          : input.wasReleased(MappedInputManager::Button::PageBack));
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const unsigned long held = input.getHeldTime();
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CasperSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              held < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CasperSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             held >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool next = tiltNext || powerTurn ||
                    (usePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                              : input.wasReleased(MappedInputManager::Button::PageForward));
  return {prev, next, tiltPrev || tiltNext};
}

// True while any physical/logical control that can emit a page-turn edge is held.
// Used to swallow bounce after a turn until the gesture fully ends.
inline bool anyPageTurnControlHeld(const MappedInputManager& input) {
  return input.isPressed(MappedInputManager::Button::PageBack) ||
         input.isPressed(MappedInputManager::Button::PageForward) ||
         input.isPressed(MappedInputManager::Button::Left) || input.isPressed(MappedInputManager::Button::Right) ||
         input.isPressed(MappedInputManager::Button::Up) || input.isPressed(MappedInputManager::Button::Down) ||
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

// Page-turn refresh with YACP-style periodic maintenance (Anti-Ghosting):
//   - Ordinary turns: FAST
//   - Every N pages (interval due):
//       X3: soft B/W reinforce (OEM AA-pre-BW mid via displayGrayscaleBase(FAST))
//           — gentle pull, no black flash (YACP)
//       X4: HALF scrub (SSD1677 has no soft bank)
//   - FORCE_SCRUB (0) / manual force-refresh: always HALF (visible hard clean)
//   - Interval "Never": FAST only (FORCE_SCRUB / manual still scrub)
//
// Soft greyscale-base (X3): also used by UiGhostPolicy menu opens (same bank).
// Async: starts FAST/HALF non-blocking when possible; X3 soft is blocking.
// Caller must not touch FB until waitRefreshComplete after async FAST/HALF.
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const int freq = SETTINGS.getRefreshFrequency();  // -1 = Never
  const bool disabled = (freq == CasperSettings::REFRESH_COUNTDOWN_DISABLED);
  const bool forceScrub = (pagesUntilFullRefresh == CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB);
  // Countdown hits 1 on the page that should maintain; also treat 0 as due.
  const bool maintenanceDue = !disabled && pagesUntilFullRefresh <= 1 && pagesUntilFullRefresh >= 0;

  // Reader-only: invert for the panel push only (FB restored after). System-wide
  // uses invertOnDisplay inside displayBuffer — never double-invert.
  const bool tempInvert = readerOnlyDarkPaint() && !renderer.getInvertOnDisplay();
  if (tempInvert) renderer.invertScreen();

  if (maintenanceDue || forceScrub) {
    // Soft interval on X3 only when not a forced hard scrub. Long-press power /
    // FORCE_SCRUB always HALF so the user sees a real flash clean.
    const bool useX3SoftReinforce = gpio.deviceIsX3() && !forceScrub;
    if (useX3SoftReinforce) {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    } else if (async) {
      renderer.displayBufferAsync(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = disabled ? CasperSettings::REFRESH_COUNTDOWN_DISABLED : freq;
    if (pagesUntilFullRefresh < 1 && !disabled) pagesUntilFullRefresh = 1;
  } else {
    if (async) {
      renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    if (!disabled && pagesUntilFullRefresh > 1) {
      pagesUntilFullRefresh--;
    }
  }

  if (tempInvert) renderer.invertScreen();
}

// Push the BW page already painted into the framebuffer, then enhance it with
// the 2-bit greyscale multipass.
//
// On X3, displayGray() is the OEM 4-level *nudge* bank. It does not replace
// the panel contents — it expects the new BW frame to already be on glass
// (and the controller RAM to be in the state displayGrayscaleBase leaves).
// Without that base step, the nudge runs against the *previous* page and the
// turn looks like nothing happened until a HALF scrub. That is exactly the
// "I have to hold power to load the next page" report, and the PAGE lines that
// said ran=1 refresh=408ms while the panel still showed the prior page.
//
// Penumbra's clock AA has always done base → greys → cleanup. This helper did
// not, and every AA-on reader path (Rivulet, Txt) went through it.
//
// Returns false when the pass could not run (storeBwBuffer needs ~48 KB in 8 KB
// chunks and fails under heap pressure). On false NOTHING has been pushed to the
// panel and the BW framebuffer is left untouched, so the caller MUST fall back to
// an ordinary refresh — otherwise the page the caller already painted never
// reaches the glass and the turn looks like it did nothing.
template <typename RenderFn>
[[nodiscard]] bool renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn,
                                       const HalDisplay::RefreshMode baseMode = HalDisplay::FAST_REFRESH) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing; falling back to BW refresh");
    return false;
  }

  // Page appears here. OEM AA-pre-BW mid settle leaves particles receptive to
  // the gray nudge that follows.
  renderer.displayGrayscaleBase(baseMode);

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
  // Rebase DTM planes from the restored BW frame and clear _inGrayscaleMode so
  // the next turn's differential BW/AA path is not fighting leftover gray RAM.
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
//
// Back release → Home. No long-press Back path (that delayed leaving the book
// and felt sluggish). Library is reached from Home / menu, not a hold-Back chord.
//
// Do not gate on held time: when the main loop is busy (section build, SD,
// first-page work), a physical short press can be sampled only on release with
// held already high because isPressed was never polled during the hold. An old
// (wasReleased && held < 500) check then no-oped — "Back needs two presses".
// activityManager/filePath kept for call-site stability (unused).
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& /*activityManager*/,
                                 const char* /*filePath*/, BackNavCallback goHome) {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    return false;
  }
  // Drain residual Back edges so the resumed Home does not treat this release
  // as Menu (minimal front-button map: short Back = Menu).
  (void)mappedInput.wasPressed(MappedInputManager::Button::Back);
  (void)mappedInput.wasReleased(MappedInputManager::Button::Back);
  goHome.fn(goHome.ctx);
  return true;
}

}  // namespace ReaderUtils
