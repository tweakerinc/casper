#include <gtest/gtest.h>

#include "ReaderRenderSync.h"

using readersync::idleWorkMayUseEngine;
using readersync::IdleWorkGate;

TEST(ReaderRenderSync, IdleWorkRunsWhenPaintIsIdle) {
  EXPECT_TRUE(idleWorkMayUseEngine(IdleWorkGate{}));
}

TEST(ReaderRenderSync, IdleWorkSkipsWhenRenderLockBusy) {
  IdleWorkGate g;
  g.renderLockBusy = true;
  EXPECT_FALSE(idleWorkMayUseEngine(g));
}

TEST(ReaderRenderSync, IdleWorkSkipsWhenRenderInProgress) {
  IdleWorkGate g;
  g.renderInProgress = true;
  EXPECT_FALSE(idleWorkMayUseEngine(g));
}

TEST(ReaderRenderSync, IdleWorkSkipsWhenBothBusy) {
  IdleWorkGate g;
  g.renderLockBusy = true;
  g.renderInProgress = true;
  EXPECT_FALSE(idleWorkMayUseEngine(g));
}
