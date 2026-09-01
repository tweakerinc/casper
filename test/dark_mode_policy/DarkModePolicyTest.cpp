#include <gtest/gtest.h>

#include "DarkModePolicy.h"

TEST(DarkModePolicy, SystemWideWhenReaderOnlyOff) {
  EXPECT_TRUE(darkmode::systemWide(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::systemWide(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_FALSE(darkmode::systemWide(/*darkOn=*/false, /*readerOnly=*/false));
}

TEST(DarkModePolicy, SkipUiGreysOnlyWhenWholeUiInverts) {
  EXPECT_TRUE(darkmode::skipUiGrayscale(/*darkOn=*/true, /*readerOnly=*/false));
  EXPECT_FALSE(darkmode::skipUiGrayscale(/*darkOn=*/true, /*readerOnly=*/true));
  EXPECT_FALSE(darkmode::skipUiGrayscale(/*darkOn=*/false, /*readerOnly=*/false));
}

TEST(DarkModePolicy, SkipReaderAaForAnyDarkMode) {
  EXPECT_TRUE(darkmode::skipReaderGrayscale(/*darkOn=*/true));
  EXPECT_FALSE(darkmode::skipReaderGrayscale(/*darkOn=*/false));
}

TEST(DarkModePolicy, HideAaToggleWhileDark) {
  EXPECT_FALSE(darkmode::showAntiAliasingSetting(/*darkOn=*/true));
  EXPECT_TRUE(darkmode::showAntiAliasingSetting(/*darkOn=*/false));
}
