#include <gtest/gtest.h>

#include "FutureChapterIndex.h"

using futureindex::decide;
using futureindex::Decision;
using futureindex::Input;
using futureindex::Limits;

namespace {

Input idleReady() {
  Input in;
  in.firstInkDone = true;
  in.currentMapComplete = true;
  in.aheadWarm = true;
  in.hasForwardTarget = true;
  in.targetHasIrCache = true;
  in.lastTurnMs = 1000;
  in.nowMs = 1000 + Limits::kQuietAfterTurnMs;
  return in;
}

}  // namespace

TEST(FutureChapterIndex, FlipThroughDoesNotStart) {
  Input in = idleReady();
  in.nowMs = in.lastTurnMs + Limits::kQuietAfterTurnMs - 1;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, StartsAfterQuietWhenCurrentMapDone) { EXPECT_EQ(decide(idleReady()), Decision::StartForward); }

TEST(FutureChapterIndex, DoesNotStartUntilCurrentMapComplete) {
  Input in = idleReady();
  in.currentMapComplete = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, DoesNotStartWhileAheadCold) {
  Input in = idleReady();
  in.aheadWarm = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, ControlHeldNeverStarts) {
  Input in = idleReady();
  in.controlHeld = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, ControlHeldAbortsResidentFuture) {
  Input in = idleReady();
  in.futureResident = true;
  in.controlHeld = true;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, MeasuresAfterPageGap) {
  Input in = idleReady();
  in.futureResident = true;
  in.lastWorkMs = in.nowMs - Limits::kPageGapMs;
  EXPECT_EQ(decide(in), Decision::MeasurePage);
}

TEST(FutureChapterIndex, WaitsOutPageGap) {
  Input in = idleReady();
  in.futureResident = true;
  in.lastWorkMs = in.nowMs - (Limits::kPageGapMs - 1);
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, FinishWhenFutureMapComplete) {
  Input in = idleReady();
  in.futureResident = true;
  in.futureMapComplete = true;
  EXPECT_EQ(decide(in), Decision::FinishRestore);
}

TEST(FutureChapterIndex, GiveUpAfterStalls) {
  Input in = idleReady();
  in.futureResident = true;
  in.futureStallTicks = Limits::kGiveUpStalls;
  in.lastWorkMs = in.nowMs - Limits::kPageGapMs;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, CapsForwardChaptersPerSitting) {
  Input in = idleReady();
  in.forwardIndexedThisSession = Limits::kMaxForwardChapters;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, CapIsOneChapterAhead) { EXPECT_EQ(Limits::kMaxForwardChapters, 1); }

TEST(FutureChapterIndex, DoesNotStartWithoutIrCache) {
  Input in = idleReady();
  in.targetHasIrCache = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, DoesNotStartWithoutForwardTarget) {
  Input in = idleReady();
  in.hasForwardTarget = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, DoesNotRetryAfterUserAbort) {
  Input in = idleReady();
  in.userAbortedThisSitting = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, TightHeapAbortsResident) {
  Input in = idleReady();
  in.futureResident = true;
  in.heapTight = true;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, TightHeapDoesNotStart) {
  Input in = idleReady();
  in.heapTight = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, WaitsStartGapAfterPreviousRestore) {
  Input in = idleReady();
  in.lastWorkMs = in.nowMs - (Limits::kStartGapMs - 1);
  EXPECT_EQ(decide(in), Decision::None);
  in.lastWorkMs = in.nowMs - Limits::kStartGapMs;
  EXPECT_EQ(decide(in), Decision::StartForward);
}

TEST(FutureChapterIndex, NoStartBeforeFirstTurn) {
  Input in = idleReady();
  in.lastTurnMs = 0;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, PageGapIsHalfCurrentChapterDuty) {
  // Current-chapter idle gap is 1500ms; future work is 3000ms (50% pace).
  EXPECT_EQ(Limits::kPageGapMs, 3000u);
  EXPECT_GT(Limits::kPageGapMs, 1500u);
}
