#pragma once

#include <IrFormat.h>

class GfxRenderer;

// The reader's layout identity: viewport, margins, font and every setting that
// changes where a line breaks.
//
// This lives outside RivuletReaderActivity because it is not the reader's alone.
// A page map (.rvpm) is only valid for the RenderKey it was built with —
// RivuletEngine::loadPageMap rejects a map whose key differs — so anything that
// *builds* maps has to compute the exact same key the reader will later present.
//
// The Home background indexer did not, and could not: the computation was a
// private method of the reader activity. Every map it wrote carried a
// default-constructed key (viewport 0x0) and was therefore discarded on load,
// while still leaving a .rvpm on disk that made the indexer skip that chapter
// forever. Sharing one definition is what makes the two agree by construction.
namespace readerkey {

struct Layout {
  rivulet::RenderKey key;
  float lineCompression = 1.0f;
  // Pixel margins the key was derived from, for callers that also paint.
  int marginL = 0;
  int marginT = 0;
  int marginR = 0;
  int marginB = 0;
};

// Derive the layout from current settings, theme metrics and renderer geometry.
// Orientation-aware: reads getOrientedViewableTRBL and the current orientation.
[[nodiscard]] Layout compute(const GfxRenderer& renderer);

}  // namespace readerkey
