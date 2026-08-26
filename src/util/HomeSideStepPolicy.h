#pragma once

// Penumbra home side keys (physical Up/Down): one gesture → one under-panel
// page (X3) or Recents row (X4).
//
// Idle HALF / clock-AA used to return from HomeActivity::loop before sampling
// buttons. A short tap is often already released that frame (pressedEvents set,
// isPressed false), so a "held" check missed it, the next gpio.update() cleared
// the latch, and the following tap's bounce advanced two pages.
namespace homeside {

inline bool idleWorkYieldsToInput(const bool held, const bool anyPressed, const bool anyReleased) {
  return held || anyPressed || anyReleased;
}

struct Request {
  bool prev = false;
  bool next = false;
  bool held = false;
  bool waitingRelease = false;
};

struct Result {
  bool accept = false;
  bool prev = false;
  bool next = false;
  bool waitingRelease = false;
};

inline Result decide(const Request& in) {
  Result out;
  out.waitingRelease = in.waitingRelease;

  if (in.prev && in.next) {
    out.waitingRelease = true;
    return out;
  }

  const bool edge = in.prev || in.next;
  if (!edge) {
    if (in.waitingRelease && !in.held) {
      out.waitingRelease = false;
    }
    return out;
  }

  if (in.waitingRelease) {
    return out;
  }

  out.accept = true;
  out.prev = in.prev;
  out.next = in.next;
  out.waitingRelease = true;
  return out;
}

}  // namespace homeside
