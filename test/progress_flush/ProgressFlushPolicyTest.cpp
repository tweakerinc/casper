#include <gtest/gtest.h>

#include "ProgressFlushPolicy.h"

using progressflush::kDebounceMs;
using progressflush::Mode;
using progressflush::shouldWriteNow;
using progressflush::State;
using progressflush::tickDue;

TEST(ProgressFlushPolicy, SamePlaceNeverWritesAndClearsPending) {
  State st;
  st.pending = true;
  st.dueMs = 1000;
  EXPECT_FALSE(shouldWriteNow(st, Mode::Now, 5000, /*samePlace=*/true));
  EXPECT_FALSE(st.pending);
}

TEST(ProgressFlushPolicy, DeferredSchedulesAndDoesNotWrite) {
  State st;
  EXPECT_FALSE(shouldWriteNow(st, Mode::Deferred, 1000, /*samePlace=*/false));
  EXPECT_TRUE(st.pending);
  EXPECT_EQ(st.dueMs, 1000UL + kDebounceMs);
  EXPECT_FALSE(tickDue(st, 1000 + kDebounceMs - 1));
  EXPECT_TRUE(tickDue(st, 1000 + kDebounceMs));
}

TEST(ProgressFlushPolicy, RepeatedDeferredResetsTimer) {
  State st;
  EXPECT_FALSE(shouldWriteNow(st, Mode::Deferred, 1000, false));
  EXPECT_FALSE(shouldWriteNow(st, Mode::Deferred, 2500, false));
  EXPECT_EQ(st.dueMs, 2500UL + kDebounceMs);
  EXPECT_FALSE(tickDue(st, 1000 + kDebounceMs));
  EXPECT_TRUE(tickDue(st, 2500 + kDebounceMs));
}

TEST(ProgressFlushPolicy, NowWritesImmediately) {
  State st;
  EXPECT_TRUE(shouldWriteNow(st, Mode::Now, 1000, false));
  EXPECT_FALSE(st.pending);
}

TEST(ProgressFlushPolicy, SleepNowAfterDeferredStillWrites) {
  State st;
  EXPECT_FALSE(shouldWriteNow(st, Mode::Deferred, 1000, false));
  // Sleep / leave / hop: Now, even 200ms later. Same-place is false after
  // persistProgressForSleep wipes lastSaved*.
  EXPECT_TRUE(shouldWriteNow(st, Mode::Now, 1200, false));
  EXPECT_FALSE(st.pending);
}

TEST(ProgressFlushPolicy, TickDoesNothingUntilDue) {
  State st;
  EXPECT_FALSE(tickDue(st, 9999));
  EXPECT_FALSE(shouldWriteNow(st, Mode::Deferred, 0, false));
  EXPECT_FALSE(tickDue(st, kDebounceMs - 1));
  EXPECT_TRUE(tickDue(st, kDebounceMs));
}
