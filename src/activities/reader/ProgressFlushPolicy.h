#pragma once

#include <cstdint>

// When to write progress.bin. Flip-through must not hit FAT every tap; a crash
// in the next few seconds can lose those pages. Sleep / leave / chapter hop
// must never skip the write — that is how a lost-progress report happens.
namespace progressflush {

static constexpr unsigned long kDebounceMs = 3000;

enum class Mode : uint8_t { Deferred, Now };

struct State {
  bool pending = false;
  unsigned long dueMs = 0;
};

// Returns true when the caller must write progress.bin now.
// samePlace: on-disk already matches RAM — drop any deferred write.
// Deferred: reset the idle timer (no write). Now: write, clear pending.
inline bool shouldWriteNow(State& st, const Mode mode, const unsigned long nowMs, const bool samePlace) {
  if (samePlace) {
    st.pending = false;
    return false;
  }
  if (mode == Mode::Deferred) {
    st.pending = true;
    st.dueMs = nowMs + kDebounceMs;
    return false;
  }
  st.pending = false;
  return true;
}

inline bool tickDue(const State& st, const unsigned long nowMs) {
  if (!st.pending) return false;
  return static_cast<long>(nowMs - st.dueMs) >= 0;
}

}  // namespace progressflush
