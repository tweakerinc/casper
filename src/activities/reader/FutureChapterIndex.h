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
// HALF scrub).
//
// v49 (782b6925) then disabled idle start entirely. Chapter hops landed with
// 1/12 est and no Loading; in-chapter idle MAP still stole taps. Idle start
// is back, but only on the last page of a sealed chapter, with Indexing on
// glass. Mid-chapter reading never evicts the IR. A PageForward during the
// swap is a chapter hop (promote), not a restore.
namespace futureindex {

// Last-page index: 3s quiet so flip-through of the final page never starts it,
// then measure as fast as the loop can poll (Indexing is already on glass).
struct Limits {
  static constexpr unsigned long kQuietAfterTurnMs = 3000;
  static constexpr unsigned long kPageGapMs = 200;
  static constexpr unsigned long kStartGapMs = 1000;
  static constexpr int kMaxForwardChapters = 1;
  static constexpr int kGiveUpStalls = 8;
  static constexpr bool kIdleForwardIndex = true;
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
  // Last page of the current chapter. Idle must not swap IR mid-chapter
  // (device v48/v49: 8s load stole in-chapter taps). Convert is allowed here
  // because Indexing is on glass.
  bool atChapterEnd = false;
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
  if (!in.atChapterEnd) return Decision::None;
  if (!in.currentMapComplete) return Decision::None;
  if (!in.aheadWarm) return Decision::None;
  if (in.userAbortedThisSitting) return Decision::None;
  if (!in.hasForwardTarget) return Decision::None;
  if (in.forwardIndexedThisSession >= Limits::kMaxForwardChapters) return Decision::None;
  if (!quietLongEnough(in, Limits::kQuietAfterTurnMs)) return Decision::None;
  if (!workGapElapsed(in, Limits::kStartGapMs)) return Decision::None;
  return Decision::StartForward;
}

}  // namespace futureindex
