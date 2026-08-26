#include <gtest/gtest.h>

#include "HomeSideStepPolicy.h"

using homeside::decide;
using homeside::idleWorkYieldsToInput;
using homeside::Request;
using homeside::Result;

TEST(HomeSideStepPolicy, IdleWorkYieldsToHeldOrEdges) {
  EXPECT_FALSE(idleWorkYieldsToInput(false, false, false));
  EXPECT_TRUE(idleWorkYieldsToInput(true, false, false));
  EXPECT_TRUE(idleWorkYieldsToInput(false, true, false));
  EXPECT_TRUE(idleWorkYieldsToInput(false, false, true));
}

TEST(HomeSideStepPolicy, AcceptsFirstPrev) {
  Request in;
  in.prev = true;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.prev);
  EXPECT_FALSE(r.next);
  EXPECT_TRUE(r.waitingRelease);
}

TEST(HomeSideStepPolicy, AcceptsFirstNext) {
  Request in;
  in.next = true;
  const Result r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.next);
  EXPECT_TRUE(r.waitingRelease);
}

TEST(HomeSideStepPolicy, BounceWhileHeldIsDropped) {
  Request in;
  in.next = true;
  Result r = decide(in);
  ASSERT_TRUE(r.accept);

  in.waitingRelease = r.waitingRelease;
  in.held = true;
  in.next = true;
  r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_TRUE(r.waitingRelease);
}

TEST(HomeSideStepPolicy, ReReleaseThenNextTapIsOneStep) {
  Request in;
  in.next = true;
  Result r = decide(in);
  ASSERT_TRUE(r.accept);

  in.waitingRelease = true;
  in.held = false;
  in.next = false;
  in.prev = false;
  r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_FALSE(r.waitingRelease);

  in.waitingRelease = r.waitingRelease;
  in.next = true;
  r = decide(in);
  EXPECT_TRUE(r.accept);
  EXPECT_TRUE(r.next);
}

TEST(HomeSideStepPolicy, AmbiguousPrevAndNextIsDropped) {
  Request in;
  in.prev = true;
  in.next = true;
  const Result r = decide(in);
  EXPECT_FALSE(r.accept);
  EXPECT_TRUE(r.waitingRelease);
}
