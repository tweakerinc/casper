#pragma once

#include <cstdint>

// Page-turn edge policy (no heap, no Arduino). PageTurnLatch in ReaderUtils.h
// is the device wrapper; this file is the decision table the host test pins.
//
// Device (DCC book 6, build 03e944bb, X3 ADC ladder): after TURN next to page 9
// the next PAGE was page 8 with no TURN next line (~1.7s later). Same pattern
// at 25→24. turnPrev did not log, so those backs were invisible. Group-2 ADC
// is Up vs Down on one pin (recorded Down≈5, Up≈2242). Releasing Down sweeps
// through the Up band; a long idle block (glyph prewarm) then lets debounce
// commit that sweep as PageBack. Sitting mid-chapter, map was already sealed,
// so this is not idle mapping.
namespace pageturn {

enum class Dir : uint8_t { None = 0, Prev, Next };

enum class Why : uint8_t {
  Idle = 0,
  Accepted,
  Swallow,
  Ambiguous,
  Opposite,
  Waiting,
  Interval,
  Back,  // Back exit in progress — page-turn edges are ghosts of that gesture
};

struct Limits {
  static constexpr unsigned long kMinIntervalMs = 180;
  // Long enough to cover the Down→Up ADC sweep + 5ms debounce; short enough
  // that a deliberate reverse after a mis-turn still feels instant.
  static constexpr unsigned long kOppositeLockMs = 400;
  // After a blocking idle bite, ignore edges until debounce can settle on
  // real idle. Covers 5ms debounce plus a couple of main-loop samples.
  static constexpr unsigned long kSwallowMs = 80;
};

struct Request {
  bool prev = false;
  bool next = false;
  bool fromTilt = false;
  bool fromTouch = false;
  bool backActive = false;
  bool held = false;
  bool waitingRelease = false;
  unsigned long swallowUntilMs = 0;
  Dir lastDir = Dir::None;
  unsigned long lastAcceptedMs = 0;
  unsigned long nowMs = 0;
};

struct Result {
  bool accept = false;
  bool prev = false;
  bool next = false;
  bool waitingRelease = false;
  Dir lastDir = Dir::None;
  unsigned long lastAcceptedMs = 0;
  Why why = Why::Idle;
};

inline char whyChar(const Why w) {
  switch (w) {
    case Why::Idle:
      return '-';
    case Why::Accepted:
      return 'k';
    case Why::Swallow:
      return 's';
    case Why::Ambiguous:
      return 'a';
    case Why::Opposite:
      return 'p';
    case Why::Waiting:
      return 'w';
    case Why::Interval:
      return 'i';
    case Why::Back:
      return 'b';
  }
  return '?';
}

inline Result decide(const Request& in) {
  Result out;
  out.prev = in.prev;
  out.next = in.next;
  out.waitingRelease = in.waitingRelease;
  out.lastDir = in.lastDir;
  out.lastAcceptedMs = in.lastAcceptedMs;

  // Back is handled on release; PageBack defaults to press. One physical Back
  // tap (or an ADC-ladder neighbour on the same group-1 pin as Left) otherwise
  // turns the page, then Saving paints over the previous page.
  if (in.backActive && (in.prev || in.next)) {
    out.why = Why::Back;
    out.prev = false;
    out.next = false;
    if (in.waitingRelease && !in.held) out.waitingRelease = false;
    return out;
  }

  const bool swallowing = in.swallowUntilMs != 0 && in.nowMs < in.swallowUntilMs;
  if (swallowing) {
    // No edge this sample — swallow is for ghost *edges*, not idle ticks.
    // Device logs (X3 eb84fe08, X4 d4f402dc): prewarm_glyphs then a burst of
    // TURN drop why=s with held=0 were the main loop polling during the 80ms
    // window, not dropped taps. Classifying them as Swallow hid real presses
    // behind noise and made "button did nothing" undiagnosable.
    if (!in.prev && !in.next) {
      out.why = Why::Idle;
      if (in.waitingRelease && !in.held) out.waitingRelease = false;
      return out;
    }
    // Device (d354dcad): MAP prewarm_glyphs then a burst of TURN drop why=s ate
    // the same-direction Next the user meant — extra presses to turn a page.
    // Swallow exists to cover the X3 ADC Down→Up ghost (opposite of lastDir).
    // lastDir None is "just opened / never turned": the first Next must land
    // (eb84fe08: PAGE then prewarm then drop why=s then a second press).
    const bool sameDir =
        (in.lastDir == Dir::Next && in.next && !in.prev) || (in.lastDir == Dir::Prev && in.prev && !in.next);
    if (in.lastDir != Dir::None && !sameDir) {
      out.why = Why::Swallow;
      out.prev = false;
      out.next = false;
      if (in.held) {
        out.waitingRelease = true;
      } else if (in.waitingRelease) {
        out.waitingRelease = false;
      }
      return out;
    }
  }

  if (!in.prev && !in.next) {
    out.why = Why::Idle;
    if (in.waitingRelease && !in.held) out.waitingRelease = false;
    return out;
  }

  // Same-frame prev+next is the ladder passing through the other band, not two
  // keys. Preferring prev (old reader loop) turned a Next into a Back.
  if (in.prev && in.next) {
    out.why = Why::Ambiguous;
    out.prev = false;
    out.next = false;
    return out;
  }

  if (in.waitingRelease) {
    out.why = Why::Waiting;
    out.prev = false;
    out.next = false;
    if (!in.held) out.waitingRelease = false;
    return out;
  }

  const Dir incoming = in.prev ? Dir::Prev : Dir::Next;
  if (in.lastDir != Dir::None && incoming != in.lastDir && in.lastAcceptedMs != 0 &&
      (in.nowMs - in.lastAcceptedMs) < Limits::kOppositeLockMs) {
    out.why = Why::Opposite;
    out.prev = false;
    out.next = false;
    return out;
  }

  const bool rateLimitOnly = in.fromTilt && !in.fromTouch && !in.held;
  if (in.lastAcceptedMs != 0 && (in.nowMs - in.lastAcceptedMs) < Limits::kMinIntervalMs) {
    out.why = Why::Interval;
    out.prev = false;
    out.next = false;
    return out;
  }

  out.accept = true;
  out.why = Why::Accepted;
  out.lastDir = incoming;
  out.lastAcceptedMs = in.nowMs;
  out.waitingRelease = !rateLimitOnly;
  return out;
}

}  // namespace pageturn
