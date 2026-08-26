#include <gtest/gtest.h>

#include "InputPollPolicy.h"

using inputpoll::decide;
using inputpoll::kFastDelayMs;
using inputpoll::kIdleDelayMs;
using inputpoll::Request;
using inputpoll::Result;

TEST(InputPollPolicy, ActiveLoopStaysFastAndLeavesClockAlone) {
  Request in;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
  EXPECT_FALSE(r.wantsFullSpeed);
}

TEST(InputPollPolicy, IdleLoopSleepsAndSavesPower) {
  Request in;
  in.idle = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kIdleDelayMs);
  EXPECT_TRUE(r.wantsPowerSaving);
}

TEST(InputPollPolicy, PowerHeldKeepsIdleCadenceFast) {
  Request in;
  in.idle = true;
  in.powerHeld = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_TRUE(r.wantsPowerSaving);
}

// The reported bug: sitting on a page for >3s puts the loop at 50ms, and a
// ~90ms tap is seen by one sample only, so it never commits.
TEST(InputPollPolicy, PendingDebounceOverridesIdleSleep) {
  Request in;
  in.idle = true;
  in.debouncePending = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
  EXPECT_TRUE(r.wantsFullSpeed);
}

TEST(InputPollPolicy, PendingDebounceBeatsPowerHeldIdleToo) {
  Request in;
  in.idle = true;
  in.powerHeld = true;
  in.debouncePending = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
}

TEST(InputPollPolicy, FastCadenceCanCommitA90msTap) {
  // InputManager needs a second matching sample more than DEBOUNCE_DELAY (5ms)
  // after the raw change. The idle cadence cannot deliver one inside a 90ms tap
  // when the first sample lands late; the fast cadence always can.
  constexpr unsigned long kDebounceDelayMs = 5;
  constexpr unsigned long kTapMs = 90;
  EXPECT_GT(kFastDelayMs, kDebounceDelayMs);
  EXPECT_LT(kFastDelayMs, kTapMs);
  EXPECT_GE(kIdleDelayMs, kTapMs / 2);
}
