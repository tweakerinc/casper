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

TEST(InputPollPolicy, IdleLoopSavesPowerButKeepsFastPoll) {
  Request in;
  in.idle = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_EQ(r.delayMs, kIdleDelayMs);
  EXPECT_TRUE(r.wantsPowerSaving);
  EXPECT_FALSE(r.wantsFullSpeed);
}

// Sitting on a page for >3s used to drop the CPU to 10 MHz and sleep 50ms.
// A long press survived; a ~90ms Next did not.
TEST(InputPollPolicy, ReaderIdleStaysFastAndFullSpeed) {
  Request in;
  in.idle = true;
  in.readerActive = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
  EXPECT_TRUE(r.wantsFullSpeed);
}

TEST(InputPollPolicy, PendingDebounceOverridesIdleSleep) {
  Request in;
  in.idle = true;
  in.debouncePending = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
  EXPECT_TRUE(r.wantsFullSpeed);
}

TEST(InputPollPolicy, PendingDebounceBeatsReaderIdleToo) {
  Request in;
  in.idle = true;
  in.readerActive = true;
  in.debouncePending = true;
  const Result r = decide(in);
  EXPECT_EQ(r.delayMs, kFastDelayMs);
  EXPECT_FALSE(r.wantsPowerSaving);
  EXPECT_TRUE(r.wantsFullSpeed);
}

TEST(InputPollPolicy, FastCadenceCanCommitA90msTap) {
  // InputManager needs a second matching sample more than DEBOUNCE_DELAY (5ms)
  // after the raw change. Both the active and idle cadences must fit two
  // samples inside a 90ms tap even when the first sample lands late.
  constexpr unsigned long kDebounceDelayMs = 5;
  constexpr unsigned long kTapMs = 90;
  EXPECT_GT(kFastDelayMs, kDebounceDelayMs);
  EXPECT_LT(kFastDelayMs, kTapMs);
  EXPECT_LT(kIdleDelayMs, kTapMs / 2);
}
