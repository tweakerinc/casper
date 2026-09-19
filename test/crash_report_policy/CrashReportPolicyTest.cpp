#include <gtest/gtest.h>

#include "CrashReportPolicy.h"

TEST(CrashReportPolicy, DumpsPanicAndWatchdog) {
  EXPECT_TRUE(crashreport::dumpOnReset(crashreport::kResetPanic));
  EXPECT_TRUE(crashreport::dumpOnReset(crashreport::kResetTaskWdt));
  EXPECT_TRUE(crashreport::dumpOnReset(crashreport::kResetIntWdt));
  EXPECT_TRUE(crashreport::dumpOnReset(crashreport::kResetWdt));
  EXPECT_TRUE(crashreport::dumpOnReset(crashreport::kResetCpuLockup));
  EXPECT_FALSE(crashreport::dumpOnReset(1));   // poweron
  EXPECT_FALSE(crashreport::dumpOnReset(3));   // sw
  EXPECT_FALSE(crashreport::dumpOnReset(8));   // deepsleep
}

TEST(CrashReportPolicy, NamesFieldResetCodes) {
  EXPECT_STREQ(crashreport::resetReasonName(4), "panic");
  EXPECT_STREQ(crashreport::resetReasonName(6), "task_wdt");
  EXPECT_STREQ(crashreport::resetReasonName(1), "poweron");
}
