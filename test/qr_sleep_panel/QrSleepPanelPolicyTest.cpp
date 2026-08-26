#include <gtest/gtest.h>

#include "QrSleepPanelPolicy.h"

using qrsleep::PanelPush;
using qrsleep::panelPush;
using qrsleep::shouldPrimeWakeBaseline;
using qrsleep::shouldPushMoonWindow;
using qrsleep::shouldPushWakeDotsWindow;

TEST(QrSleepPanelPolicy, GreysOnGlassSkipSleepMoonPush) {
  EXPECT_EQ(panelPush(true), PanelPush::Skip);
  EXPECT_FALSE(shouldPushMoonWindow(true));
}

TEST(QrSleepPanelPolicy, BwPanelPushesSleepMoon) {
  EXPECT_EQ(panelPush(false), PanelPush::Fast);
  EXPECT_TRUE(shouldPushMoonWindow(false));
}

TEST(QrSleepPanelPolicy, WakeNeverPushesDots) {
  EXPECT_FALSE(shouldPushWakeDotsWindow(false));
  EXPECT_FALSE(shouldPushWakeDotsWindow(true));
}

TEST(QrSleepPanelPolicy, OnlyX4PrimesWakeBaseline) {
  EXPECT_TRUE(shouldPrimeWakeBaseline(true));
  EXPECT_FALSE(shouldPrimeWakeBaseline(false));
}
