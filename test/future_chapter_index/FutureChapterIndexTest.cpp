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

TEST(FutureChapterIndex, IdleForwardIndexOff) { EXPECT_FALSE(Limits::kIdleForwardIndex); }

TEST(FutureChapterIndex, DoesNotStartAtChapterEndWhenIdleOff) { EXPECT_EQ(decide(idleAtEnd()), Decision::None); }

TEST(FutureChapterIndex, DoesNotStartMidChapterEvenWhenMapDone) { EXPECT_EQ(decide(idleReady()), Decision::None); }

TEST(FutureChapterIndex, ControlHeldNeverStarts) {
  Input in = idleAtEnd();
  in.controlHeld = true;
  EXPECT_EQ(decide(in), Decision::None);
}

TEST(FutureChapterIndex, ForwardHeldPromotesResidentFuture) {
  // Device 9206393c: Next on last page during Indexing restored spine 25
  // instead of hopping to 26.
  Input in = idleAtEnd();
  in.futureResident = true;
  in.controlHeld = true;
  in.forwardHeld = true;
  EXPECT_EQ(decide(in), Decision::Promote);
}

TEST(FutureChapterIndex, OtherControlAbortsResidentFuture) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.controlHeld = true;
  in.forwardHeld = false;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
}

TEST(FutureChapterIndex, FirstPageIsEnoughThenRestore) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.futureHasFirstPage = true;
  EXPECT_EQ(decide(in), Decision::FinishRestore);
}

TEST(FutureChapterIndex, FinishWhenFutureMapComplete) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.futureMapComplete = true;
  EXPECT_EQ(decide(in), Decision::FinishRestore);
}

TEST(FutureChapterIndex, MeasuresOnlyWithoutFirstPage) {
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

TEST(FutureChapterIndex, GiveUpAfterStalls) {
  Input in = idleAtEnd();
  in.futureResident = true;
  in.futureStallTicks = Limits::kGiveUpStalls;
  in.lastWorkMs = in.nowMs - Limits::kPageGapMs;
  EXPECT_EQ(decide(in), Decision::AbortRestore);
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

TEST(FutureChapterIndex, CapIsOneChapterAhead) { EXPECT_EQ(Limits::kMaxForwardChapters, 1); }

TEST(FutureChapterIndex, NoStartBeforeFirstTurn) {
  Input in = idleAtEnd();
  in.lastTurnMs = 0;
  EXPECT_EQ(decide(in), Decision::None);
}
