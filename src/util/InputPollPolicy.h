#pragma once

// Main-loop button poll cadence.
//
// InputManager commits a raw state change only after two consecutive matching
// samples (DEBOUNCE_DELAY = 5ms). After IDLE_POWER_SAVING_MS with no *committed*
// button event the loop used to drop the CPU to 10 MHz and sleep 50ms. A normal
// ~90ms tap is then seen by one sample: the next already reads released, the
// raw state flips, lastDebounceTime resets, and the press never commits. Lost
// presses also never reset the inactivity timer, so the slow cadence sticks.
// On glass that is "I pressed next a few times, then a long press turned the
// page" with no TURN line — a long hold is the only gesture that survives two
// 50ms samples at 10 MHz (device log 388a52d2, X4 DCC ch45, 41s gap then TURN).
//
// isDebouncePending() is the SDK's signal that a raw change has not committed
// yet. Poll fast and keep the CPU at full speed while it is set, and also
// while the reader is open: analogRead of the Xteink ADC ladder plus idle
// footnote/map bites become multi-hundred-ms stalls at 10 MHz and swallow the
// same taps. The 50ms idle sleep is gone everywhere; deep sleep still owns
// long inactivity.
namespace inputpoll {

// Matches the pre-existing responsive cadence; > DEBOUNCE_DELAY so the next
// sample can commit. Idle uses the same value — a 50ms sleep cannot deliver
// two matching samples inside a ~90ms tap.
constexpr unsigned long kFastDelayMs = 10;
constexpr unsigned long kIdleDelayMs = kFastDelayMs;

struct Request {
  bool idle = false;             // past IDLE_POWER_SAVING_MS with no input
  bool debouncePending = false;  // raw sample differs from the committed state
  bool readerActive = false;     // book is open: never drop the CPU
};

struct Result {
  unsigned long delayMs = kFastDelayMs;
  // Tri-state so the caller only touches the CPU clock when the cadence needs
  // it: idle on Home asks for low power, a pending edge or an open book
  // insists on full speed, and the plain active path leaves whatever the
  // activity handler set.
  bool wantsPowerSaving = false;
  bool wantsFullSpeed = false;
};

inline Result decide(const Request& in) {
  Result out;
  // A pending edge and an open book both outrank power saving. Dropping the
  // clock here is what makes ADC samples miss a short tap.
  if (in.debouncePending || in.readerActive) {
    out.wantsFullSpeed = true;
    return out;
  }
  if (!in.idle) return out;
  out.wantsPowerSaving = true;
  return out;
}

}  // namespace inputpoll
