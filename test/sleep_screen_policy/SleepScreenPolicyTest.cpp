#include <gtest/gtest.h>

#include "SleepScreenPolicy.h"

TEST(SleepScreenPolicy, FactoryLightBecomesQuickResume) {
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(sleepscreen::kLight, /*alreadyMigrated=*/false),
            sleepscreen::kQuickResume);
}

TEST(SleepScreenPolicy, DarkCoverCustomBlankStay) {
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(sleepscreen::kDark, false), sleepscreen::kDark);
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(/*cover=*/3, false), 3);
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(/*custom=*/2, false), 2);
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(/*blank=*/5, false), 5);
}

TEST(SleepScreenPolicy, AlreadyMigratedLightIsHonored) {
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(sleepscreen::kLight, /*alreadyMigrated=*/true),
            sleepscreen::kLight);
}

TEST(SleepScreenPolicy, AlreadyQuickResumeStays) {
  EXPECT_EQ(sleepscreen::migrateFactoryLightToQuickResume(sleepscreen::kQuickResume, false), sleepscreen::kQuickResume);
}
