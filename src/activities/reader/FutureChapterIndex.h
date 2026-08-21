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
// v49 (782b6925) disabled idle start. It came back last-page-only with
// Indexing on glass. Device (DCC book 6 ch.13 last page, 9206393c): that
// still converted spine 26 in 16.7s (cache=0), walked all 44 pages (~63s),
// then restored spine 25 in 13.4s. `activity_slow` 19s then 17s. PageForward
// during the swap restored the old chapter instead of promoting — Next
// "froze and did not want to load". Chapter hops already paint Loading in
// turnNext. Idle swap is off; PageForward while a future IR is resident is
// a hop (Promote), not a restore. If a first page is already laid out,
// FinishRestore immediately — never walk the rest of the future chapter.
namespace futureindex {

struct Limits {
  static constexpr unsigned long kQuietAfterTurnMs = 3000;
  static constexpr unsigned long kPageGapMs = 200;
  static constexpr unsigned long kStartGapMs = 1000;
  static constexpr int kMaxForwardChapters = 1;
  static constexpr int kGiveUpStalls = 8;
  // Off: last-page idle convert blocked the reader for tens of seconds and
  // ate the chapter hop. turnNext already shows Loading and loads the next
  // spine on the tap that actually wants it.
  static constexpr bool kIdleForwardIndex = false;
};

enum class Decision : uint8_t {
  None,
  StartForward,   // evict current IR, load next unmapped spine
  MeasurePage,    // extendPageMap(1) on the resident future spine
  FinishRestore,  // enough future work — persist and restore current
  AbortRestore,   // user input, stall, or tight heap — restore current
  Promote,        // PageForward during the swap: this IS the chapter hop
};

struct Input {
  bool futureResident = false;
  bool currentMapComplete = false;
  bool aheadWarm = false;
  bool firstInkDone = false;
  bool controlHeld = false;
  bool forwardHeld = false;  // PageForward — hop, do not restore
  bool futureMapComplete = false;
  bool futureHasFirstPage = false;  // page 1 laid out; do not walk the chapter
  bool heapTight = false;
  // Tap during a swap: do not StartForward again until the reader changes spine.
  bool userAbortedThisSitting = false;
  // Last page of the current chapter. Idle must not swap IR mid-chapter
  // (device v48/v49: 8s load stole in-chapter taps).
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
  // PageForward while the next chapter is already in the engine is the hop.
  if (in.futureResident && in.forwardHeld) return Decision::Promote;
  if (in.controlHeld && in.futureResident) return Decision::AbortRestore;
  if (in.controlHeld) return Decision::None;
  if (!in.firstInkDone) return Decision::None;
  if (in.heapTight && in.futureResident) return Decision::AbortRestore;
  if (in.heapTight) return Decision::None;

  if (in.futureResident) {
    if (in.futureMapComplete) return Decision::FinishRestore;
    // First page is enough to persist IR. Walking the rest (device: 44 pages,
    // ~63s) left the user on Indexing until a 13s restore dumped them back.
    if (in.futureHasFirstPage) return Decision::FinishRestore;
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
