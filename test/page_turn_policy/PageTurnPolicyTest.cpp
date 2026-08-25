#include <gtest/gtest.h>

#include "PageTurnPolicy.h"

using pageturn::decide;
using pageturn::Dir;
using pageturn::Limits;
using pageturn::Request;
using pageturn::Result;
using pageturn::Why;

namespace {

Request forwardAt(const unsigned long now) {
  Request in;
  in.next = true;
  in.nowMs = now;
  return in;
}

Request backAt(const unsigned long now) {
  Request in;
  in.prev = true;
  in.nowMs = now;
  return in;
}

}  // namespace

TEST(PageTurnPolicy, AcceptsFirstForward) {
  const Result r = decide(forwardAt(1000));
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.next);
  EXPECT_FALSE(r.prev);
  EXPECT_EQ(r.lastDir, Dir::Next);
  EXPECT_TRUE(r.waitingRelease);
  EXPECT_EQ(r.why, Why::Accepted);
}

TEST(PageTurnPolicy, AcceptsFirstBack) {
  const Result r = decide(backAt(1000));
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.prev);
  EXPECT_EQ(r.lastDir, Dir::Prev);
}

TEST(PageTurnPolicy, RejectsAmbiguousPrevAndNext) {
  Request in = forwardAt(1000);
  in.prev = true;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_FALSE(r.prev);
  EXPECT_FALSE(r.next);
  EXPECT_EQ(r.why, Why::Ambiguous);
}

TEST(PageTurnPolicy, DropsOppositeWithinLock) {
  Result first = decide(forwardAt(1000));
  Request back;
  back.prev = true;
  back.lastDir = first.lastDir;
  back.lastAcceptedMs = first.lastAcceptedMs;
  back.nowMs = 1000 + Limits::kOppositeLockMs - 1;
  const Result r = decide(back);
  EXPECT_FALSE(r.accept);
  EXPECT_EQ(r.why, Why::Opposite);
  EXPECT_EQ(r.lastDir, Dir::Next);
}

TEST(PageTurnPolicy, AllowsOppositeAfterLock) {
  Result first = decide(forwardAt(1000));
  Request back;
  back.prev = true;
  back.lastDir = first.lastDir;
  back.lastAcceptedMs = first.lastAcceptedMs;
  back.nowMs = 1000 + Limits::kOppositeLockMs;
  const Result r = decide(back);
  EXPECT_TRUE(r.accept);
  EXPECT_EQ(r.lastDir, Dir::Prev);
}

TEST(PageTurnPolicy, WaitingReleaseDropsSameGesture) {
  Request in = forwardAt(1200);
  in.waitingRelease = true;
  in.held = true;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_EQ(r.why, Why::Waiting);
  EXPECT_TRUE(r.waitingRelease);
}

TEST(PageTurnPolicy, WaitingReleaseClearsWhenUp) {
  Request in;
  in.waitingRelease = true;
  in.held = false;
  in.nowMs = 1300;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_FALSE(r.waitingRelease);
  EXPECT_EQ(r.why, Why::Idle);
}

TEST(PageTurnPolicy, SwallowDropsEdgeAndArmsWaitIfHeld) {
  // Opposite of lastDir (ADC ghost Back after Next) is what swallow exists for.
  Request in = backAt(2000);
  in.lastDir = Dir::Next;
  in.swallowUntilMs = 2000 + Limits::kSwallowMs;
  in.held = true;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_FALSE(r.prev);
  EXPECT_EQ(r.why, Why::Swallow);
  EXPECT_TRUE(r.waitingRelease);
}

TEST(PageTurnPolicy, SwallowAllowsSameDirNext) {
  // Device d354dcad: prewarm_glyphs then why=s ate the Next the user meant.
  Request in = forwardAt(2000);
  in.lastDir = Dir::Next;
  in.lastAcceptedMs = 1500;
  in.swallowUntilMs = 2000 + Limits::kSwallowMs;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.next);
  EXPECT_EQ(r.why, Why::Accepted);
}

TEST(PageTurnPolicy, SwallowDoesNotDropFirstTapAfterOpen) {
  // lastDir None: open then Next. Swallow after prewarm must not eat that tap.
  Request in = forwardAt(2000);
  in.swallowUntilMs = 2000 + Limits::kSwallowMs;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.next);
  EXPECT_EQ(r.why, Why::Accepted);
  EXPECT_EQ(r.lastDir, Dir::Next);
}

TEST(PageTurnPolicy, SwallowIdleHasNoEdge) {
  // Idle ticks during the swallow window must not log as why=s.
  Request in;
  in.lastDir = Dir::Next;
  in.lastAcceptedMs = 1500;
  in.swallowUntilMs = 2000 + Limits::kSwallowMs;
  in.nowMs = 2000;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_EQ(r.why, Why::Idle);
}

TEST(PageTurnPolicy, SwallowExpires) {
  Request in = backAt(2000 + Limits::kSwallowMs);
  in.swallowUntilMs = 2000 + Limits::kSwallowMs;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_EQ(r.why, Why::Accepted);
}

TEST(PageTurnPolicy, IntervalRejectsRapidSameDir) {
  Result first = decide(forwardAt(1000));
  Request again = forwardAt(1000 + Limits::kMinIntervalMs - 1);
  again.lastDir = first.lastDir;
  again.lastAcceptedMs = first.lastAcceptedMs;
  const Result r = decide(again);
  EXPECT_FALSE(r.accept);
  EXPECT_EQ(r.why, Why::Interval);
}

TEST(PageTurnPolicy, TiltDoesNotHoldWaitingRelease) {
  Request in = forwardAt(1000);
  in.fromTilt = true;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_FALSE(r.waitingRelease);
}

TEST(PageTurnPolicy, WhyCharsAreStableForLogs) {
  EXPECT_EQ(pageturn::whyChar(Why::Idle), '-');
  EXPECT_EQ(pageturn::whyChar(Why::Swallow), 's');
  EXPECT_EQ(pageturn::whyChar(Why::Ambiguous), 'a');
  EXPECT_EQ(pageturn::whyChar(Why::Opposite), 'p');
}
