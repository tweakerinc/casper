#pragma once

#include <cstdint>

// Idle policy for crawling the *next* chapter onto SD while the current one
// is on glass.
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
// v49 (782b6925) disabled idle start. Last-page-only + Indexing (9206393c)
// still converted spine 26 in 16.7s, walked all 44 pages (~63s), then restored
// spine 25 in 13.4s. The last page is often a sentence or two, so the 3s quiet
// never runs there. v57 (d354dcad) turned idle off: hops worked (3.2s then
// 2.4s) but every chapter boundary still paid Loading on the tap.
//
// Product: silent crawl *mid-chapter* (glass stays on the current page). Convert
// next IR + lay out page 1 onto SD, restore current. Never walk the future
// chapter. PageForward during the swap is a hop (Promote) only when the held
// place was already the last page; mid-chapter Next restores then turns.
namespace futureindex {

struct Limits {
  static constexpr unsigned long kQuietAfterTurnMs = 3000;
  static constexpr unsigned long kPageGapMs = 200;
  static constexpr unsigned long kStartGapMs = 1000;
  static constexpr int kMaxForwardChapters = 1;
  static constexpr int kGiveUpStalls = 8;
  // Idle start does not lend the 48KB framebuffer (glass still holds this
  // page). ChapterLoader's convert floor is 48KB maxAlloc / 60KB free — below
  // that, ingestHtml + requireFull retry ran 21s then failed (device 8fa5688f
  // spine 27, maxA=31732, activity_slow 38269ms). Resident abort stays on the
  // tighter heapTight flag so page-0 layout is not cancelled for 48KB.
  static constexpr uint32_t kMinMaxAllocToStart = 48 * 1024;
  static constexpr uint32_t kMinFreeToStart = 60 * 1024;
  // .rvir already on SD: loadIr + goToStart, no ingestHtml. Device 300ec1c0
  // skipped at maxA=36852 and 47092 (need 49152) after a sitting that already
  // had IR — layout-only would have prefetched page 1. Do not lower the
  // convert floor (31KB ingestHtml was the 21s freeze).
  static constexpr uint32_t kMinMaxAllocLayoutOnly = 24 * 1024;
  static constexpr uint32_t kMinFreeLayoutOnly = 32 * 1024;
  // On: layout page 1 of the *already-converted* next spine while the user is
  // still reading this one. Never idle-convert (no .rvir): device e38a3a71
  // StartForward cache=0 ran ingestHtml 16s + restore 8s (activity_slow 33s).
  static constexpr bool kIdleForwardIndex = true;
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
  // Enough contiguous heap to convert without lending the FB. StartForward only.
  bool heapOkToConvert = false;
  // Tap during a swap: do not StartForward again until the reader changes spine.
  bool userAbortedThisSitting = false;
  // HTML footnote scan still owed after first ink. StartForward in that window
  // stacked a 1.7s ZIP/HTML read with an IR swap (device bcff8a47: NOTE then
  // the session died, EN rst=1).
  bool footnoteScanPending = false;
  // Last page of the reader's held place (not the future spine in the engine).
  // Promote only then — mid-chapter PageForward must restore, not hop.
  bool heldAtChapterEnd = false;
  bool atChapterEnd = false;
  bool hasForwardTarget = false;
  int futureStallTicks = 0;
  int forwardIndexedThisSession = 0;
  unsigned long nowMs = 0;
  // Real forward page turn only. Open/onEnter must leave this 0 or StartForward
  // runs while the user is still looking at the first page.
  unsigned long lastTurnMs = 0;
  unsigned long lastWorkMs = 0;
};

inline bool quietLongEnough(const Input& in, const unsigned long needMs) {
  if (in.lastTurnMs == 0) return false;  // never turned forward this sitting
  return (in.nowMs - in.lastTurnMs) >= needMs;
}

inline bool workGapElapsed(const Input& in, const unsigned long needMs) {
  if (in.lastWorkMs == 0) return true;
  return (in.nowMs - in.lastWorkMs) >= needMs;
}

inline Decision decide(const Input& in) {
  // PageForward on the last page while the next chapter is already in the
  // engine is the hop. Mid-chapter Next must not jump a whole spine.
  if (in.futureResident && in.forwardHeld && in.heldAtChapterEnd) return Decision::Promote;
  if (in.controlHeld && in.futureResident) return Decision::AbortRestore;
  if (in.controlHeld) return Decision::None;
  if (!in.firstInkDone) return Decision::None;
  if (in.heapTight && in.futureResident) return Decision::AbortRestore;
  if (in.heapTight) return Decision::None;

  if (in.futureResident) {
    if (in.futureMapComplete) return Decision::FinishRestore;
    // First page is enough to persist IR + .rvpg. Walking the rest (device:
    // 44 pages, ~63s) left the user on Indexing until a 13s restore.
    if (in.futureHasFirstPage) return Decision::FinishRestore;
    if (in.futureStallTicks >= Limits::kGiveUpStalls) return Decision::AbortRestore;
    if (!workGapElapsed(in, Limits::kPageGapMs)) return Decision::None;
    return Decision::MeasurePage;
  }

  if (!Limits::kIdleForwardIndex) return Decision::None;
  if (!in.currentMapComplete) return Decision::None;
  if (!in.aheadWarm) return Decision::None;
  if (in.userAbortedThisSitting) return Decision::None;
  if (in.footnoteScanPending) return Decision::None;
  if (!in.hasForwardTarget) return Decision::None;
  if (in.forwardIndexedThisSession >= Limits::kMaxForwardChapters) return Decision::None;
  if (!quietLongEnough(in, Limits::kQuietAfterTurnMs)) return Decision::None;
  if (!workGapElapsed(in, Limits::kStartGapMs)) return Decision::None;
  if (!in.heapOkToConvert) return Decision::None;
  return Decision::StartForward;
}

}  // namespace futureindex
