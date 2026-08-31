#include <gtest/gtest.h>

#include "PageMap.h"
#include "util/OrientationPageRemap.h"

TEST(OrientationPageRemap, StaysPutWhenCountsMatch) { EXPECT_EQ(orientpage::remap(40, 80, 80), 40); }

TEST(OrientationPageRemap, DoesNotScaleIntoAHeuristicGuess) {
  // Device eb84fe08: complete landscape 23/46, setRenderKey heuristic ~81,
  // live portrait was 53. Scaling landed at 41/53 (near the end).
  EXPECT_EQ(orientpage::remap(23, 46, 81), 23);
  EXPECT_NE(orientpage::remap(23, 46, 81), 41);
}

TEST(OrientationPageRemap, KeepsIndexWhenShrinking) { EXPECT_EQ(orientpage::remap(40, 80, 50), 40); }

TEST(OrientationPageRemap, OnePageLiveMapClampsToStart) {
  // After loadSpine the live map is often 1 page. Clamp, do not scale.
  EXPECT_EQ(orientpage::remap(80, 100, 1), 0);
}

TEST(OrientationPageRemap, DeepPlaceDoesNotJumpToStart) {
  EXPECT_EQ(orientpage::remap(80, 100, 60), 59);
  EXPECT_NE(orientpage::remap(80, 100, 60), 0);
}

TEST(OrientationPageRemap, ZeroAndNegativesStayAtStart) {
  EXPECT_EQ(orientpage::remap(0, 100, 60), 0);
  EXPECT_EQ(orientpage::remap(-3, 100, 60), 0);
}

TEST(OrientationPageRemap, ClampsToLastPage) { EXPECT_EQ(orientpage::remap(99, 100, 10), 9); }

TEST(PageIndexForCursor, EmptyIsInvalid) {
  using rivulet::IrCursor;
  EXPECT_EQ(rivulet::pageIndexForCursor(nullptr, 0, IrCursor{}), -1);
}

TEST(PageIndexForCursor, MidChapterStaysOnContainingPage) {
  using rivulet::IrCursor;
  const IrCursor starts[] = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 4, IrCursor{0, 0, 0}), 0);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 4, IrCursor{15, 2, 8}), 1);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 4, IrCursor{20, 0, 0}), 2);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 4, IrCursor{40, 0, 0}), 3);
}

TEST(PageIndexForCursor, RunAndByteBreakTies) {
  using rivulet::IrCursor;
  const IrCursor starts[] = {{5, 0, 0}, {5, 3, 0}, {5, 3, 12}};
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 3, IrCursor{5, 2, 99}), 0);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 3, IrCursor{5, 3, 0}), 1);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 3, IrCursor{5, 3, 11}), 1);
  EXPECT_EQ(rivulet::pageIndexForCursor(starts, 3, IrCursor{5, 3, 12}), 2);
}
