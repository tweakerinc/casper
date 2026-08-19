#pragma once

#include <cstdint>
#include <string>

#include "ChapterIr.h"
#include "IrFormat.h"
#include "LaidOutPage.h"
#include "PageMap.h"
#include "PageLayouter.h"

class GfxRenderer;

namespace rivulet {

// Facade: one chapter open, Tier A page window, Tier B IR on SD, thin page maps.
//
// v1: Latin, Source Serif 4 + Literata ladders, no RTL / auto-turn.
// Paint via GfxRenderer; integration with Casper activities is separate.
class RivuletEngine {
 public:
  // legacy-style look-ahead for the thin page index (not painted pages).
  static constexpr int kMapAheadPages = 5;
  // Idle tick budget: a few map pages without monopolizing the main loop.
  // When the map is far behind the heuristic estimate, tickIdlePageMap asks for
  // a larger burst so the status-bar "~" settles sooner (classic had a real LUT).
  static constexpr int kIdleMapPagesPerTick = 4;
  static constexpr int kIdleMapPagesCatchUp = 10;

  // When the render key changes, invalidate page map + paint window (F).
  void setRenderKey(const RenderKey& key);
  void setLineCompression(const float lc) { lineCompression_ = lc; }
  [[nodiscard]] const RenderKey& renderKey() const { return key_; }

  // Convert HTML → IR, optionally persist to irPath (.rvir).
  // imageRendering: 0=Display, 1=Placeholder, 2=Suppress (Settings → Images).
  bool ingestHtml(const char* html, size_t len, const char* irPathSave, bool armDropCapFirstPara = false,
                  uint8_t imageRendering = 0);
  bool ingestHtml(const char* html, const char* irPathSave, bool armDropCapFirstPara = false,
                  uint8_t imageRendering = 0) {
    if (!html) return false;
    size_t n = 0;
    while (html[n]) ++n;
    return ingestHtml(html, n, irPathSave, armDropCapFirstPara, imageRendering);
  }

  // Load Tier B IR from SD.
  bool loadIr(const char* irPath);

  // Load page map if key matches and cursors fit IR; otherwise false.
  bool loadPageMap(const char* mapPath);
  bool savePageMap(const char* mapPath) const;
  // If a loaded map is marked complete but the last page does not end the
  // chapter under live layout, clear complete and keep starts as a lower bound.
  // Call after loadPageMap + setRenderKey (needs renderer metrics).
  bool scrubStaleCompleteMap(const GfxRenderer& renderer);

  // Build full page map (idle / after open). Requires renderer for metrics.
  bool buildPageMap(const GfxRenderer& renderer);

  // Extend thin page map by up to maxPages (no paint change). Returns true if work done.
  bool extendPageMap(const GfxRenderer& renderer, int maxPages);
  // Ensure map has starts through currentPage + aheadPages (or complete).
  bool ensureMapAhead(const GfxRenderer& renderer, int aheadPages = kMapAheadPages);

  // Navigation
  // maxWalkPages caps progressive map fill so interactive calls cannot freeze.
  // Resume may pass a larger budget (with yield inside).
  bool goToPage(const GfxRenderer& renderer, int pageIndex, int maxWalkPages = 64);
  bool nextPage(const GfxRenderer& renderer);
  bool prevPage(const GfxRenderer& renderer);
  bool goToStart(const GfxRenderer& renderer);
  // True last page of this chapter IR (legacy-style full layout walk).
  // Map-hit only when complete map's last page is verified atChapterEnd.
  // Otherwise pure layoutPage walk from start — never lands mid-chapter.
  // Caller should hold Loading UI; yields inside; may take a few seconds.
  bool goToLastPage(const GfxRenderer& renderer, int maxWalkPages = 1024);
  // Prev-chapter landing when a full end-walk fails: prefer a complete map, else
  // the deepest known map page, else an estimate-based goToPage. Always stays
  // inside the already-loaded chapter (never returns false just to force the
  // caller to walk earlier spines back to book start).
  bool goToBestEffortLastPage(const GfxRenderer& renderer, int maxWalkPages = 1024);

  // Layout current page into laidOut_ (call before paint).
  bool ensureLaidOut(const GfxRenderer& renderer);

  // Lay out the paint-ahead page (the one after the current page) if it is not
  // already warm. Call from the idle tick, NOT from a page turn.
  //
  // Page turns used to do this synchronously right after promoting ahead_ into
  // laidOut_, which meant every forward turn blocked on a full layout of a page
  // the reader had not asked for yet. Deferring it is purely a scheduling
  // change: if the reader turns again before idle gets a chance, nextPage()
  // simply takes its existing slow path and lays the page out then — the same
  // single layout it would have paid anyway. So a turn is never slower than
  // before, and is one whole layout faster whenever the device had a moment.
  //
  // Returns true if it laid a page out (i.e. did real work this tick).
  bool warmAheadPage(const GfxRenderer& renderer);
  // Bidirectional indexing: keep the previous page's layout in RAM so page-back
  // is a move (same idea as ahead_ for forward). Warmed on idle.
  bool warmBehindPage(const GfxRenderer& renderer);
  [[nodiscard]] bool aheadWarm() const { return aheadValid_; }
  [[nodiscard]] bool behindWarm() const { return behindValid_; }

  // Paint laid-out page at origin (margins applied by caller or via key).
  void paint(GfxRenderer& renderer, int originX, int originY) const;

  [[nodiscard]] bool hasChapter() const { return !chapter_.empty(); }
  [[nodiscard]] int currentPage() const { return currentPage_; }
  // Exact if map complete; else estimate from IR (+ map extrapolation while walking).
  [[nodiscard]] int chapterPageCount(const GfxRenderer* rendererForEstimate = nullptr) const;
  // How many measure-only pages the idle tick should lay out this pass.
  // Larger when the map is far behind the estimate so "~N" settles faster.
  [[nodiscard]] int idleMapPagesThisTick(const GfxRenderer* rendererForEstimate = nullptr) const;
  [[nodiscard]] const LaidOutPage& page() const { return laidOut_; }
  [[nodiscard]] const ChapterIr& chapter() const { return chapter_; }
  // Mutable chapter for post-convert image probe (dims) before first layout.
  [[nodiscard]] ChapterIr& chapterMutable() { return chapter_; }
  [[nodiscard]] bool mapComplete() const { return map_.complete(); }
  [[nodiscard]] int mapKnownPages() const { return map_.knownPages(); }
  // Drop the thin page map only (keep chapter IR). Used when a stale/incomplete
  // .rvpm makes deep resume fail and would otherwise fall back to page 0.
  void invalidatePageMap();

  // Classic-style page paint cache on SD (deserialize + paint on revisit).
  // dir = rivulet chapter folder; empty disables. Files: p{N}.rvpg
  void setPageCacheDir(const char* dir);
  void clearPageCacheDir() { pageCacheDir_.clear(); }
  // Idle: layout one upcoming page and write .rvpg (not kept in RAM). Heap-gated.
  // Returns true if it did real work this call.
  bool idlePrefetchPageCache(const GfxRenderer& renderer, int maxForward = 3);

  void clear();

 private:
  LayoutParams makeParams(const GfxRenderer& renderer) const;
  // Same params with measureOnly set: for page-map walks, which read only the
  // resulting end cursor and discard the spans. See LayoutParams::measureOnly.
  LayoutParams makeMeasureParams(const GfxRenderer& renderer) const;
  bool layoutAtCursor(const GfxRenderer& renderer, const IrCursor& c);
  void seedMapIfEmpty();
  // markComplete only if known page count is plausible vs IR estimate.
  void markMapCompleteIfPlausible(const GfxRenderer& renderer);
  bool tryLoadPageCache(int pageIndex);
  void savePageCache(int pageIndex) const;
  bool pageCachePath(int pageIndex, char* out, size_t outSz) const;

  RenderKey key_{};
  float lineCompression_ = 1.0f;
  ChapterIr chapter_{};
  PageMap map_{};
  LaidOutPage laidOut_{};
  LaidOutPage ahead_{};   // Tier A: one page painted ahead (consumed on nextPage)
  LaidOutPage behind_{};  // Tier A: one page behind (consumed on prevPage)
  int currentPage_ = 0;
  bool laidOutValid_ = false;
  bool aheadValid_ = false;
  bool behindValid_ = false;
  // EMA of the incomplete-map estimate so status-bar "~N" does not jump every
  // idle tick (classic Section::estimatedTotalPages used the same idea).
  mutable float smoothedEstimate_ = 0.0f;
  mutable int smoothedAtKnown_ = -1;
  std::string pageCacheDir_;
};

}  // namespace rivulet
