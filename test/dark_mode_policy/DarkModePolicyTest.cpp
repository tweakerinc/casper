#include <gtest/gtest.h>

#include "DarkModePolicy.h"

TEST(DarkModePolicy, SystemWideWhenReaderOnlyOff) {
  EXPECT_TRUE(darkmode::systemWide(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::systemWide(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_FALSE(darkmode::systemWide(/*darkOn=*/false, /*readerOnly=*/false));
}

TEST(DarkModePolicy, PreserveCoverPolarityOnlyWhenWholeUiInverts) {
  EXPECT_TRUE(darkmode::preserveCoverPolarity(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::preserveCoverPolarity(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_FALSE(darkmode::preserveCoverPolarity(/*darkOn=*/false, /*readerOnly=*/false));
}

TEST(DarkModePolicy, SkipReaderAaForAnyDarkMode) {
  EXPECT_TRUE(darkmode::skipReaderGrayscale(/*darkOn=*/true));
  EXPECT_FALSE(darkmode::skipReaderGrayscale(/*darkOn=*/false));
}

TEST(DarkModePolicy, HideAaToggleWhileDark) {
  EXPECT_FALSE(darkmode::showAntiAliasingSetting(/*darkOn=*/true));
  EXPECT_TRUE(darkmode::showAntiAliasingSetting(/*darkOn=*/false));
}

TEST(DarkModePolicy, ClockAaStillSkippedInWholeUiDark) {
  EXPECT_TRUE(darkmode::skipUiGrayscale(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::skipUiGrayscale(/*darkOn=*/true, /*readerOnly=*/true));
}

TEST(DarkModePolicy, CoverGreysRunInWholeUiDark) {
  EXPECT_FALSE(darkmode::skipCoverGrayscale(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::skipCoverGrayscale(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_FALSE(darkmode::skipCoverGrayscale(/*darkOn=*/false, /*readerOnly=*/false));
}

TEST(DarkModePolicy, OpeningAndMenuNeedHalfInWholeUiDark) {
  EXPECT_TRUE(darkmode::statusCueNeedsHalf(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::statusCueNeedsHalf(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_TRUE(darkmode::menuOpenNeedsHalf(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::menuOpenNeedsHalf(/*darkOn=*/true, /*readerOnly=*/true));
}
