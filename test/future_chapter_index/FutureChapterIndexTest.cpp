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
  in.lastTurnMs = 1000;
  in.nowMs = 1000 + Limits::kQuietAfterTurnMs;
  return in;
}

Input idleAtEnd() {
  Input in = idleReady();
  in.atChapterEnd = true;
  return in;
}

}  // namespace

TEST(FutureChapterIndex, FlipThroughDoesNotStart) {
  Input in = idleAtEnd();
  in.nowMs = in.lastTurnMs + Limits::kQuietAfterTurnMs - 1;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, IdleForwardIndexOn) { EXPECT_TRUE(Limits::kIdleForwardIndex); }

TEST(FutureChapterIndex, DoesNotStartMidChapterEvenWhenMapDone) { EXPECT_EQ(decide(idleReady()), Decision::None); }

TEST(FutureChapterIndex, StartsAfterQuietAtChapterEnd) { EXPECT_EQ(decide(idleAtEnd()), Decision::StartForward); }

TEST(FutureChapterIndex, DoesNotStartUntilCurrentMapComplete) {
  Input in = idleAtEnd();
  in.currentMapComplete = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, DoesNotStartWhileAheadCold) {
  Input in = idleAtEnd();
  in.aheadWarm = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, ControlHeldNeverStarts) {
  Input in = idleAtEnd();
  in.controlHeld = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, ControlHeldAbortsResidentFuture) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.controlHeld = true;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, MeasuresAfterPageGap) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.lastWorkMs = in.nowMs - Limits::kPageGapMs;
  EXPECT_EQ(decide(in), Decision::MeasurePage);
}

TEST(FutureChapterIndex, WaitsOutPageGap) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.lastWorkMs = in.nowMs - (Limits::kPageGapMs - 1);
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, FinishWhenFutureMapComplete) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.futureMapComplete = true;
  EXPECT_EQ(decide(in), Decision::FinishRestore);
}

TEST(FutureChapterIndex, GiveUpAfterStalls) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.futureStallTicks = Limits::kGiveUpStalls;
  in.lastWorkMs = in.nowMs - Limits::kPageGapMs;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, CapsForwardChaptersPerSitting) {
  Input in = idleAtEnd();
  in.forwardIndexedThisSession = Limits::kMaxForwardChapters;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, CapIsOneChapterAhead) { EXPECT_EQ(Limits::kMaxForwardChapters, 1); }

TEST(FutureChapterIndex, DoesNotStartWithoutForwardTarget) {
  Input in = idleAtEnd();
  in.hasForwardTarget = false;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, DoesNotRetryAfterUserAbort) {
  Input in = idleAtEnd();
  in.userAbortedThisSitting = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, TightHeapAbortsResident) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.heapTight = true;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, TightHeapDoesNotStart) {
  Input in = idleAtEnd();
  in.heapTight = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, WaitsStartGapAfterPreviousRestore) {
  Input in = idleAtEnd();
  in.lastWorkMs = in.nowMs - (Limits::kStartGapMs - 1);
  EXPECT_EQ(decide(in), Decision::None);
  in.lastWorkMs = in.nowMs - Limits::kStartGapMs;
  EXPECT_EQ(decide(in), Decision::StartForward);
}

TEST(FutureChapterIndex, NoStartBeforeFirstTurn) {
  Input in = idleAtEnd();
  in.lastTurnMs = 0;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, PageGapIsTightWhileIndexingShows) {
  // Indexing is on glass at last page — measure as fast as the loop can poll.
  EXPECT_EQ(Limits::kPageGapMs, 200u);
  EXPECT_LT(Limits::kPageGapMs, 1500u);
}
