#pragma once

#include <algorithm>

// Remap a 0-based in-chapter page across a viewport / render-key change.
//
// The live page map after loadSpine is often a single laid-out page (nowCount=1).
// Scaling keepPage/keepCount * 1 lands at chapter start — the "flip orientation
// and jump back many pages" report. Callers must pass the *heuristic* page count
// for the new viewport (chapterPageCountForEta after setRenderKey, before the
// chapter is cleared), never the 1-page live map.
namespace orientpage {

inline int remap(const int page, const int oldCount, const int newCount) {
  if (page <= 0) return 0;
  const int oldC = std::max(1, oldCount);
  const int newC = std::max(1, newCount);
  if (oldC == newC) return std::clamp(page, 0, newC - 1);
  const int mapped =
      static_cast<int>((static_cast<float>(page) / static_cast<float>(oldC)) * static_cast<float>(newC) + 0.5f);
  return std::clamp(mapped, 0, newC - 1);
}

}  // namespace orientpage
