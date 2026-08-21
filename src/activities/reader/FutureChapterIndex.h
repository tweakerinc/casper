#pragma once

#include <cstdint>

// Idle policy for mapping *future* chapters after the current one is sealed.
//
// Why a separate unit: the reader used to swap the resident chapter out and
// walk another spine in one blocking pass. That made page turns miss paints.
// This policy is the gate the idle tick must obey so future-chapter work can
// never outrank a tap. The activity owns SD / IR / restore; this file is
// timing and priority only (no heap, no Arduino).
namespace futureindex {

// Current-chapter idle map uses 2s quiet + 1.5s between pages. Future work is
// half that duty: longer sit-still before we evict, longer gap between pages.
// 8s quiet matches the old next-spine warm gate so flip-through never starts it.
struct Limits {
  static constexpr unsigned long kQuietAfterTurnMs = 8000;
  static constexpr unsigned long kPageGapMs = 3000;
  static constexpr unsigned long kStartGapMs = 4000;
  static constexpr int kMaxForwardChapters = 3;
  static constexpr int kGiveUpStalls = 8;
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

  if (!in.currentMapComplete) return Decision::None;
  if (!in.aheadWarm) return Decision::None;
  if (in.forwardIndexedThisSession >= Limits::kMaxForwardChapters) return Decision::None;
  if (!quietLongEnough(in, Limits::kQuietAfterTurnMs)) return Decision::None;
  if (!workGapElapsed(in, Limits::kStartGapMs)) return Decision::None;
  return Decision::StartForward;
}

}  // namespace futureindex
