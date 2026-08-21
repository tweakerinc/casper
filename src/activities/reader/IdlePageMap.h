#pragma once

#include <cstdint>

// Idle policy for mapping the *open* chapter until its page map is sealed.
//
// CrossPoint 1.5 / 1.6 (classic EpubReader): loop() sampled input first, then
// `buildSomeMore(2)` while `section->isPartial()` OR the live page count was
// still inside BUILD_WINDOW_AHEAD (5). A partial (unfinalized) chapter kept
// ticking until the LUT was exact — that is why 1.5/1.6 showed a real
// `27/36` long before the reader reached page 27. Subsequent opens loaded
// the complete section.bin and skipped the walk.
//
// Rivulet v49/v50 (device DCC book 6, build 2b77af72): idle mapping stopped
// once `known > current + kIdleMapAheadPages` (cap=1). Sitting on page 16
// with known=21 logged `MAP window … cap=1` for 37s and never measured
// another page. Status stayed `27/34 est` → `28/35 est` and sometimes
// dropped to 33 because the heuristic EMA was still guessing.
//
// This unit is timing and priority only (no heap, no Arduino). The activity
// still does one layoutPage per bite (~1.7s on X3) so a tap is not stacked
// behind a 10-page burst. Abort-during-layout is a separate GPIO peek.
namespace idlemap {

struct Limits {
  // 0 = keep measuring until mapComplete() (classic isPartial keep-ticking).
  // A positive cap is the old v49/v50 window and must not ship as the default.
  static constexpr int kAheadPages = 0;
  static constexpr int kPagesPerBite = 1;
  // Rapid paging must not start a 1.7s measure. Classic prewarm used 400ms;
  // 2s matches the previous idle-map quiet so a flip-through stays snappy.
  static constexpr unsigned long kQuietAfterTurnMs = 2000;
  // One loop() between bites so mappedInput.update() can see a press.
  // v48 used 1500ms and the chapter still finished, but slowly; 250ms is
  // enough to sample input without stretching a 15-page tail to 50s.
  static constexpr unsigned long kBiteGapMs = 250;
};

struct Input {
  bool mapComplete = false;
  bool controlHeld = false;
  bool firstInkDone = false;
  int knownPages = 0;
  int currentPage = 0;  // 0-based
  unsigned long nowMs = 0;
  unsigned long lastTurnMs = 0;
  unsigned long lastWorkMs = 0;
};

inline bool shouldMeasure(const Input& in) {
  if (!in.firstInkDone) return false;
  if (in.controlHeld) return false;
  if (in.mapComplete) return false;
  if (in.lastTurnMs != 0 && (in.nowMs - in.lastTurnMs) < Limits::kQuietAfterTurnMs) return false;
  if (in.lastWorkMs != 0 && (in.nowMs - in.lastWorkMs) < Limits::kBiteGapMs) return false;
  if (Limits::kAheadPages > 0 && in.knownPages > in.currentPage + Limits::kAheadPages) return false;
  return true;
}

}  // namespace idlemap
