#include <gtest/gtest.h>

#include "QrSleepPanelPolicy.h"

using qrsleep::PanelPush;
using qrsleep::panelPush;
using qrsleep::shouldPushMoonWindow;
using qrsleep::shouldPushWakeDotsWindow;

TEST(QrSleepPanelPolicy, GreysOnGlassSkipSleepAndWakePushes) {
  EXPECT_EQ(panelPush(true), PanelPush::Skip);
  EXPECT_FALSE(shouldPushMoonWindow(true));
  EXPECT_FALSE(shouldPushWakeDotsWindow(true));
}

TEST(QrSleepPanelPolicy, BwPanelPushesMoonAndWakeDots) {
  EXPECT_EQ(panelPush(false), PanelPush::Fast);
  EXPECT_TRUE(shouldPushMoonWindow(false));
  EXPECT_TRUE(shouldPushWakeDotsWindow(false));
}
