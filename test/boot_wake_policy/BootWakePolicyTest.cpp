#include <gtest/gtest.h>

#include "BootWakePolicy.h"

TEST(BootWakePolicy, SleepClearedBootScreenIsPowerWake) {
  EXPECT_TRUE(bootwake::x4PowerOnIsSleepWake(/*showBootScreen=*/false));
}

TEST(BootWakePolicy, ArmedBootScreenIsResetOrCold) {
  EXPECT_FALSE(bootwake::x4PowerOnIsSleepWake(/*showBootScreen=*/true));
}

TEST(BootWakePolicy, X4BatterySleepResumes) {
  EXPECT_TRUE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/true, /*gpioOther=*/false,
                                               /*showBootScreen=*/false, /*x4PowerOnEnResetShape=*/true));
}

TEST(BootWakePolicy, X4EnResetWhileAwakeSplashes) {
  EXPECT_FALSE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/true, /*gpioOther=*/false,
                                                /*showBootScreen=*/true, /*x4PowerOnEnResetShape=*/true));
}

TEST(BootWakePolicy, X3GpioDeepSleepResumes) {
  EXPECT_TRUE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/true, /*gpioOther=*/false,
                                               /*showBootScreen=*/false, /*x4PowerOnEnResetShape=*/false));
}

TEST(BootWakePolicy, X3PowerOnAfterSleepResumesNotSplash) {
  // GPIO13 SD-rail cut can come back as POWERON/Other. Last shutdown was sleep.
  EXPECT_TRUE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/false, /*gpioOther=*/true,
                                               /*showBootScreen=*/false, /*x4PowerOnEnResetShape=*/false));
}

TEST(BootWakePolicy, X3EnResetWhileAwakeStillSplashes) {
  EXPECT_FALSE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/false, /*gpioOther=*/true,
                                                /*showBootScreen=*/true, /*x4PowerOnEnResetShape=*/false));
}

TEST(BootWakePolicy, ColdBootOtherIsNotSleepWake) {
  EXPECT_FALSE(bootwake::isPowerButtonSleepWake(/*gpioPowerButton=*/false, /*gpioOther=*/true,
                                                /*showBootScreen=*/true, /*x4PowerOnEnResetShape=*/false));
}
