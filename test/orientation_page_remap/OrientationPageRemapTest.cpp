#include <gtest/gtest.h>

#include "util/OrientationPageRemap.h"

TEST(OrientationPageRemap, StaysPutWhenCountsMatch) { EXPECT_EQ(orientpage::remap(40, 80, 80), 40); }

TEST(OrientationPageRemap, ScalesIntoATallerPortrait) {
  // Landscape ~50 pages → portrait ~80. Mid-chapter stays mid-chapter.
  EXPECT_EQ(orientpage::remap(25, 50, 80), 40);
}

TEST(OrientationPageRemap, ScalesIntoAWiderLandscape) { EXPECT_EQ(orientpage::remap(40, 80, 50), 25); }

TEST(OrientationPageRemap, OnePageLiveMapWouldJumpToStart) {
  // After loadSpine the live map is often 1 page. Scaling against that
  // denominator is the orientation-flip regression — callers must pass
  // chapterPageCountForEta for the new viewport instead.
  EXPECT_EQ(orientpage::remap(80, 100, 1), 0);
}

TEST(OrientationPageRemap, RealEstimateKeepsADeepPlace) {
  EXPECT_EQ(orientpage::remap(80, 100, 60), 48);
  EXPECT_NE(orientpage::remap(80, 100, 60), 0);
}

TEST(OrientationPageRemap, ZeroAndNegativesStayAtStart) {
  EXPECT_EQ(orientpage::remap(0, 100, 60), 0);
  EXPECT_EQ(orientpage::remap(-3, 100, 60), 0);
}

TEST(OrientationPageRemap, ClampsToLastPage) { EXPECT_EQ(orientpage::remap(99, 100, 10), 9); }
