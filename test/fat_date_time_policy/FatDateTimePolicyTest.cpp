#include <gtest/gtest.h>

#include "FatDateTimePolicy.h"

TEST(FatDateTimePolicy, ParsesFirmwareDate) {
  const auto fw = fatdate::firmwareFallback("Sep 19 2026");
  EXPECT_EQ(fw.year, 2026);
  EXPECT_EQ(fw.month, 9);
  EXPECT_EQ(fw.day, 19);
  EXPECT_EQ(fw.hour, 0);
  EXPECT_EQ(fw.minute, 0);
}

TEST(FatDateTimePolicy, ParsesSpacePaddedDay) {
  const auto fw = fatdate::firmwareFallback("Sep  9 2026");
  EXPECT_EQ(fw.year, 2026);
  EXPECT_EQ(fw.month, 9);
  EXPECT_EQ(fw.day, 9);
}

TEST(FatDateTimePolicy, Year2024IsNotPlausibleIn2026) {
  EXPECT_FALSE(fatdate::yearIsPlausible(2024, 2026));
  EXPECT_TRUE(fatdate::yearIsPlausible(2025, 2026));
  EXPECT_TRUE(fatdate::yearIsPlausible(2026, 2026));
  EXPECT_FALSE(fatdate::yearIsPlausible(1980, 2026));
}

TEST(FatDateTimePolicy, StaleRtcVsFirmwareIsTheFourWeeksAgoCase) {
  EXPECT_TRUE(fatdate::rtcBehindFirmware(2026, 8, 22, 2026, 9, 19));
  EXPECT_FALSE(fatdate::rtcBehindFirmware(2026, 9, 19, 2026, 9, 19));
  EXPECT_FALSE(fatdate::rtcBehindFirmware(2026, 9, 20, 2026, 9, 19));
}

TEST(FatDateTimePolicy, SeedsWhenRtcMissingDateOrBehindFirmware) {
  const auto fw = fatdate::firmwareFallback("Sep 19 2026");
  EXPECT_FALSE(fatdate::shouldSeedRtc(/*rtcPresent=*/false, true, 2026, 9, 19, fw));
  EXPECT_TRUE(fatdate::shouldSeedRtc(/*rtcPresent=*/true, /*readOk=*/false, 0, 0, 0, fw));
  EXPECT_TRUE(fatdate::shouldSeedRtc(true, true, 2024, 1, 1, fw));
  EXPECT_TRUE(fatdate::shouldSeedRtc(true, true, 2026, 8, 22, fw));
  EXPECT_FALSE(fatdate::shouldSeedRtc(true, true, 2026, 9, 19, fw));
}

TEST(FatDateTimePolicy, ThisBinaryIsNotTheOld2024Fallback) {
  const auto fw = fatdate::firmwareFallback(__DATE__);
  EXPECT_GE(fw.year, 2025);
  EXPECT_NE((fw.year == 2024 && fw.month == 1 && fw.day == 1), true);
}
