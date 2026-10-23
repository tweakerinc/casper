#pragma once

namespace readersync {

// Loop-task idle work vs the render task. GfxRenderer, ChapterIr windows, and
// PageMap are not thread-safe. ESP32 time-slices equal-priority tasks, so paint
// can run mid-layout unless the loop holds RenderLock (or skips while busy).
struct IdleWorkGate {
  bool renderLockBusy = false;
  bool renderInProgress = false;
};

inline bool idleWorkMayUseEngine(const IdleWorkGate& g) { return !g.renderLockBusy && !g.renderInProgress; }

}  // namespace readersync
