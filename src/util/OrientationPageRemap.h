#pragma once

#include <algorithm>

// Remap a 0-based in-chapter page across a viewport / render-key change.
//
// Callers that still have the chapter IR must resume at the page's IR cursor
// (RivuletEngine::resumeAtCursor) — that keeps the same text on glass.
// This helper is only the fallback when no cursor exists (UI-released IR).
//
// Device eb84fe08 Flip Orientation: complete landscape 23/46 scaled into a
// post-setRenderKey heuristic of ~81 → 41/~81. Live portrait was 53 pages, so
// the reader jumped to 41/53 (~77% through) then a second flip 41/53 → 47/~61
// and goToPage(47) failed (landed at chapter end). Never scale into the new
// viewport heuristic; keep the old index clamped to the new count.
namespace orientpage {

inline int remap(const int page, const int /*oldCount*/, const int newCount) {
  if (page <= 0) return 0;
  const int newC = std::max(1, newCount);
  return std::clamp(page, 0, newC - 1);
}

}  // namespace orientpage
