#pragma once

// Main-loop button poll cadence.
//
// InputManager commits a raw state change only after two consecutive matching
// samples (DEBOUNCE_DELAY = 5ms). After IDLE_POWER_SAVING_MS with no *committed*
// button event the loop drops the CPU clock and sleeps 50ms per iteration, so a
// normal ~90ms tap is seen by exactly one sample: the next sample already reads
// released, the raw state flips again, lastDebounceTime resets, and the press
// never commits. Nothing is logged because nothing was ever detected, and a lost
// press also never resets the inactivity timer — so the slow cadence persists
// and the next taps are lost the same way. On glass that is "I pressed next
// three or four times before the page turned" with no TURN line in the capture.
//
// isDebouncePending() is the SDK's signal that a raw change has not committed
// yet. Poll fast while it is set so the pending edge gets its second sample.
namespace inputpoll {

// Matches the pre-existing responsive cadence; > DEBOUNCE_DELAY so the next
// sample can commit.
constexpr unsigned long kFastDelayMs = 10;
constexpr unsigned long kIdleDelayMs = 50;

struct Request {
  bool idle = false;             // past IDLE_POWER_SAVING_MS with no input
  bool debouncePending = false;  // raw sample differs from the committed state
  bool powerHeld = false;        // power is down: release edge must stay crisp
};

struct Result {
  unsigned long delayMs = kFastDelayMs;
  // Tri-state so the caller only touches the CPU clock when the cadence needs
  // it: idle asks for low power, a pending edge insists on full speed, and the
  // plain active path leaves whatever the activity handler set.
  bool wantsPowerSaving = false;
  bool wantsFullSpeed = false;
};

inline Result decide(const Request& in) {
  Result out;
  // A pending edge outranks power saving. Dropping the clock and sleeping 50ms
  // here is exactly what loses the tap.
  if (in.debouncePending) {
    out.wantsFullSpeed = true;
    return out;
  }
  if (!in.idle) return out;
  out.wantsPowerSaving = true;
  out.delayMs = in.powerHeld ? kFastDelayMs : kIdleDelayMs;
  return out;
}

}  // namespace inputpoll
