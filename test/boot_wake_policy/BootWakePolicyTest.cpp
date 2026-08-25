#include <gtest/gtest.h>

#include "BootWakePolicy.h"

TEST(BootWakePolicy, SleepClearedBootScreenIsPowerWake) {
  EXPECT_TRUE(bootwake::x4PowerOnIsSleepWake(/*showBootScreen=*/false));
}

TEST(BootWakePolicy, ArmedBootScreenIsResetOrCold) {
  EXPECT_FALSE(bootwake::x4PowerOnIsSleepWake(/*showBootScreen=*/true));
}
