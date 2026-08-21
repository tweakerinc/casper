#pragma once

#include <cstdint>

// Idle policy for mapping *future* chapters after the current one is sealed.
//
// Why a separate unit: the reader used to swap the resident chapter out and
// walk another spine in one blocking pass. That made page turns miss paints.
// This policy is the gate the idle tick must obey so future-chapter work can
// never outrank a tap. The activity owns SD / IR / restore; this file is
// timing and priority only (no heap, no Arduino).
//
// Device (DCC book 6, build 6d817495): idle StartForward evicted spine 23,
// converted 24 in 14.5s (cache=0), then after every tap reloaded the same
// IR in 8.5s. Cap 3 then crawled 25/26 and failed 27/28 (19–21s) — main loop
// blocked 12–35s (`activity_slow`). Restore while the turn key was still down
// made getHeldTime() exceed 400ms and fired the side long-press (Dark Mode /
// HALF scrub). Idle work is therefore: at most the immediate next chapter,
// only from an existing .rvir, never again this sitting after a user abort,
// and never a fresh HTML convert.
namespace futureindex {

// Current-chapter idle map uses 2s quiet + 1.5s between pages. Future work is
// half that duty: longer sit-still before we evict, longer gap between pages.
// 8s quiet matches the old next-spine warm gate so flip-through never starts it.
struct Limits {
  static constexpr unsigned long kQuietAfterTurnMs = 8000;
  static constexpr unsigned long kPageGapMs = 3000;
  static constexpr unsigned long kStartGapMs = 4000;
  static constexpr int kMaxForwardChapters = 1;
  static constexpr int kGiveUpStalls = 8;
  // Device v48 (3f4b541d): even cap-1 + skip-uncached still allows an 8s
  // cached IR swap the moment the current map seals. One chapter in RAM —
  // idle must not evict it. Abort/measure/restore stay for a swap already live.
  static constexpr bool kIdleForwardIndex = false;
};

enum class Decision : uint8_t {
  None,
  StartForward,   // evict current IR, load next unmapped spine
  MeasurePage,    // extendPageMap(1) on the resident future spine
  FinishRestore,  // future map complete — persist .rvpm and restore current
  AbortRestore,   // user input, stall, or tight heap — restore current
};

struct Input {
  bool futureResident = false;
  bool currentMapComplete = false;
  bool aheadWarm = false;
  bool firstInkDone = false;
  bool controlHeld = false;
  bool futureMapComplete = false;
  bool heapTight = false;
  // Tap during a swap: do not StartForward again until the reader changes spine.
  bool userAbortedThisSitting = false;
  // Immediate next chapter has a complete .rvir. Idle must not HTML-convert
  // (device: cache=0 load 14–21s, then restore 8s, loop frozen).
  bool targetHasIrCache = false;
  bool hasForwardTarget = false;
  int futureStallTicks = 0;
  int forwardIndexedThisSession = 0;
  unsigned long nowMs = 0;
  unsigned long lastTurnMs = 0;
  unsigned long lastWorkMs = 0;
};

inline bool quietLongEnough(const Input& in, const unsigned long needMs) {
  if (in.lastTurnMs == 0) return false;  // never turned — wait for first ink settle
  return (in.nowMs - in.lastTurnMs) >= needMs;
}

inline bool workGapElapsed(const Input& in, const unsigned long needMs) {
  if (in.lastWorkMs == 0) return true;
  return (in.nowMs - in.lastWorkMs) >= needMs;
}

inline Decision decide(const Input& in) {
  if (in.controlHeld && in.futureResident) return Decision::AbortRestore;
  if (in.controlHeld) return Decision::None;
  if (!in.firstInkDone) return Decision::None;
  if (in.heapTight && in.futureResident) return Decision::AbortRestore;
  if (in.heapTight) return Decision::None;

  if (in.futureResident) {
    if (in.futureMapComplete) return Decision::FinishRestore;
    if (in.futureStallTicks >= Limits::kGiveUpStalls) return Decision::AbortRestore;
    if (!workGapElapsed(in, Limits::kPageGapMs)) return Decision::None;
    return Decision::MeasurePage;
  }

  if (!Limits::kIdleForwardIndex) return Decision::None;
  if (!in.currentMapComplete) return Decision::None;
  if (!in.aheadWarm) return Decision::None;
  if (in.userAbortedThisSitting) return Decision::None;
  if (!in.hasForwardTarget) return Decision::None;
  if (!in.targetHasIrCache) return Decision::None;
  if (in.forwardIndexedThisSession >= Limits::kMaxForwardChapters) return Decision::None;
  if (!quietLongEnough(in, Limits::kQuietAfterTurnMs)) return Decision::None;
  if (!workGapElapsed(in, Limits::kStartGapMs)) return Decision::None;
  return Decision::StartForward;
}

}  // namespace futureindex
