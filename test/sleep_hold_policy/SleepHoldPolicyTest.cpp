#include <gtest/gtest.h>

#include "SleepHoldPolicy.h"

TEST(SleepHoldPolicy, X3CutsGpio13ForSdRail) { EXPECT_TRUE(sleephold::cutGpio13InSleep(sleephold::Device::X3)); }

TEST(SleepHoldPolicy, X4CutsGpio13ForBatteryLatch) { EXPECT_TRUE(sleephold::cutGpio13InSleep(sleephold::Device::X4)); }

TEST(SleepHoldPolicy, OtherBoardsDoNotDriveGpio13) {
  EXPECT_FALSE(sleephold::cutGpio13InSleep(sleephold::Device::Other));
}

TEST(SleepHoldPolicy, IsolateRequiresASecondHold) { EXPECT_TRUE(sleephold::reassertHoldAfterIsolate()); }

TEST(SleepHoldPolicy, CutPinIsGpio13) { EXPECT_EQ(sleephold::kXteinkC3CutGpio, 13); }
