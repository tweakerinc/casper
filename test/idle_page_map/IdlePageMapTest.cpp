#include <gtest/gtest.h>

#include "IdlePageMap.h"

using idlemap::Input;
using idlemap::Limits;
using idlemap::shouldMeasure;

namespace {

Input sittingStill() {
  Input in;
  in.firstInkDone = true;
  in.mapComplete = false;
  in.knownPages = 21;
  in.currentPage = 15;  // page 16 on glass
  in.lastTurnMs = 1000;
  in.nowMs = 1000 + Limits::kQuietAfterTurnMs;
  in.lastWorkMs = 0;
  return in;
}

}  // namespace

TEST(IdlePageMap, SealsWholeChapterNotAWindow) { EXPECT_EQ(Limits::kAheadPages, 0); }

TEST(IdlePageMap, OnePagePerBite) { EXPECT_EQ(Limits::kPagesPerBite, 1); }

TEST(IdlePageMap, MeasuresWhenIdlePastQuiet) { EXPECT_TRUE(shouldMeasure(sittingStill())); }

TEST(IdlePageMap, DoesNotMeasureDuringFlipThrough) {
  Input in = sittingStill();
  in.nowMs = in.lastTurnMs + Limits::kQuietAfterTurnMs - 1;
  EXPECT_FALSE(shouldMeasure(in));
}

TEST(IdlePageMap, DoesNotMeasureWhileHeld) {
  Input in = sittingStill();
  in.controlHeld = true;
  EXPECT_FALSE(shouldMeasure(in));
}

TEST(IdlePageMap, DoesNotMeasureWhenComplete) {
  Input in = sittingStill();
  in.mapComplete = true;
  EXPECT_FALSE(shouldMeasure(in));
}

TEST(IdlePageMap, WaitsBiteGap) {
  Input in = sittingStill();
  in.lastWorkMs = in.nowMs - (Limits::kBiteGapMs - 1);
  EXPECT_FALSE(shouldMeasure(in));
}

TEST(IdlePageMap, MeasuresAfterBiteGap) {
  Input in = sittingStill();
  in.lastWorkMs = in.nowMs - Limits::kBiteGapMs;
  EXPECT_TRUE(shouldMeasure(in));
}

TEST(IdlePageMap, KeepsMeasuringFarAheadOfReadHead) {
  // v50 cap=1 stopped here (known=21, page=16) and never sealed the chapter.
  Input in = sittingStill();
  in.knownPages = 21;
  in.currentPage = 15;
  EXPECT_TRUE(shouldMeasure(in));
  in.knownPages = 28;
  in.currentPage = 26;
  EXPECT_TRUE(shouldMeasure(in));
}
