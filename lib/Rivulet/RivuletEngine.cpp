#include "RivuletEngine.h"

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "FontLadder.h"
#include "HtmlToIr.h"

namespace rivulet {

void RivuletEngine::clear() {
  chapter_.clear();
  map_.clear();
  laidOut_.clear();
  ahead_.clear();
  behind_.clear();
  currentPage_ = 0;
  laidOutValid_ = false;
  aheadValid_ = false;
  behindValid_ = false;
  smoothedEstimate_ = 0.0f;
  smoothedAtKnown_ = -1;
  // Keep pageCacheDir_/pageCacheSpine_ — loadSpine resets spine before layout.
}

void RivuletEngine::invalidatePageMap() {
  map_.clear();
  map_.setRenderKey(key_);
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();
  // Keep laidOut_/currentPage_ — caller will goToPage/goToStart next.
  smoothedEstimate_ = 0.0f;
  smoothedAtKnown_ = -1;
}

void RivuletEngine::setPageCacheDir(const char* dir) {
  if (!dir || !*dir) {
    pageCacheDir_.clear();
    pageCacheDirReady_ = false;
    return;
  }
  if (pageCacheDir_ != dir) {
    pageCacheDir_ = dir;
    pageCacheDirReady_ = false;
  }
}

// ensureDirectoryExists scans the parent directory, and the page-cache folder is
// touched on every page save / idle prefetch. Doing it once per directory keeps
// that off the page-turn path (the folder fills up with .rvpg files, so the scan
// gets slower the more we cache).
void RivuletEngine::ensurePageCacheDir() const {
  if (pageCacheDir_.empty() || pageCacheDirReady_) return;
  Storage.ensureDirectoryExists(pageCacheDir_.c_str());
  pageCacheDirReady_ = true;
}

void RivuletEngine::setPageCacheSpine(const int spineIndex) {
  if (pageCacheSpine_ == spineIndex) return;
  pageCacheSpine_ = spineIndex;
  // Never keep ahead/behind paint from another spine — those are full glyph
  // pages and would flash the wrong chapter under the new status label.
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();
  laidOutValid_ = false;
}

uint32_t RivuletEngine::pageCacheKeyFp() const {
  return static_cast<uint32_t>(key_.fontId) ^ (static_cast<uint32_t>(key_.viewportW) << 16) ^
         (static_cast<uint32_t>(key_.viewportH) << 8) ^ key_.flags ^ key_.pad ^
         (static_cast<uint32_t>(key_.lineCompressionQ8) << 4);
}

bool RivuletEngine::fillPageCachePath(const int spineIndex, const int pageIndex, char* out, const size_t outSz) const {
  if (pageCacheDir_.empty() || spineIndex < 0 || !out || outSz < 32 || pageIndex < 0) return false;
  // Filename embeds spine + key fingerprint. Older shared p{N}_{key}.rvpg files
  // are ignored (different name) so chapter 5 page-0 cache cannot paint on ch7.
  const int n =
      std::snprintf(out, outSz, "%s/s%d_p%d_%08x.rvpg", pageCacheDir_.c_str(), spineIndex, pageIndex, pageCacheKeyFp());
  return n > 0 && static_cast<size_t>(n) < outSz;
}

bool RivuletEngine::pageCachePath(const int pageIndex, char* out, const size_t outSz) const {
  return fillPageCachePath(pageCacheSpine_, pageIndex, out, outSz);
}

bool RivuletEngine::hasPageCache(const int spineIndex, const int pageIndex) const {
  char path[220];
  if (!fillPageCachePath(spineIndex, pageIndex, path, sizeof(path))) return false;
  return Storage.exists(path);
}

bool RivuletEngine::loadOrphanPageCache(const int spineIndex, const int pageIndex) {
  if (!chapter_.empty()) return false;
  setPageCacheSpine(spineIndex);
  return tryLoadPageCache(pageIndex);
}

bool RivuletEngine::tryLoadPageCache(const int pageIndex) {
  char path[220];
  if (!pageCachePath(pageIndex, path, sizeof(path))) return false;
  LaidOutPage tmp;
  if (!tmp.loadFromFile(path, key_, pageIndex)) return false;
  // Reject caches whose IR cursors cannot belong to the loaded chapter (defense
  // in depth if a path ever collides again).
  const uint32_t blocks = static_cast<uint32_t>(chapter_.blockCount());
  if (blocks > 0 && (tmp.start.blockIndex >= blocks || tmp.end.blockIndex > blocks)) {
    Storage.remove(path);
    return false;
  }
  // Must agree with the page-map cursor when present — stale IR/map would skip text.
  if (map_.hasPage(pageIndex) && tmp.start != map_.pageStart(pageIndex)) {
    Storage.remove(path);
    return false;
  }
  if (!map_.hasPage(pageIndex)) {
    // Only accept orphan cache for page 0 into an empty map (seed continuity).
    if (!(pageIndex == 0 && map_.empty())) return false;
    map_.setRenderKey(key_);
    map_.resetWithStart(tmp.start);
  }
  laidOut_ = std::move(tmp);
  laidOutValid_ = true;
  currentPage_ = pageIndex;
  if (!laidOut_.atChapterEnd) {
    const int nextIdx = pageIndex + 1;
    if (!map_.hasPage(nextIdx)) {
      map_.pushPageStart(laidOut_.end);
    } else if (map_.pageStart(nextIdx) != laidOut_.end) {
      map_.setPageStart(nextIdx, laidOut_.end);
    }
  }
  LOG_DBG("RVEN", "page cache HIT p=%d spans=%u", pageIndex, static_cast<unsigned>(laidOut_.spans.size()));
  return true;
}

void RivuletEngine::savePageCache(const int pageIndex) const {
  if (!laidOutValid_ || pageIndex < 0 || pageCacheDir_.empty()) return;
  if (ESP.getMaxAllocHeap() < 12 * 1024 || ESP.getFreeHeap() < 20 * 1024) return;
  ensurePageCacheDir();
  char path[220];
  if (!pageCachePath(pageIndex, path, sizeof(path))) return;
  if (laidOut_.saveToFile(path, key_, pageIndex)) {
    LOG_DBG("RVEN", "page cache SAVE p=%d spans=%u", pageIndex, static_cast<unsigned>(laidOut_.spans.size()));
  }
}

bool RivuletEngine::idlePrefetchPageCache(const GfxRenderer& renderer, const int maxForward) {
  if (pageCacheDir_.empty() || !laidOutValid_ || maxForward < 1) return false;
  // Stricter than turn-path saves: idle work must not risk OOM while the user
  // is just reading. Fail soft — next idle tick retries.
  if (ESP.getMaxAllocHeap() < 18 * 1024 || ESP.getFreeHeap() < 32 * 1024) return false;

  for (int d = 1; d <= maxForward; ++d) {
    const int target = currentPage_ + d;
    if (target < 0) continue;
    char path[220];
    if (!pageCachePath(target, path, sizeof(path))) continue;
    if (Storage.exists(path)) continue;  // already on SD

    // Need a map start for this page — extend measure-only if needed.
    // Keep this small: this runs on the idle tick, and 8 extends × 4 targets was
    // up to 32 layouts per tick (CPU burn while "idle" = battery, the opposite of
    // the goal). Anything deeper belongs to the normal map-extension step below.
    int guard = 0;
    while (!map_.hasPage(target) && !map_.complete() && guard++ < 2) {
      if (!extendPageMap(renderer, 1)) break;
    }
    if (!map_.hasPage(target)) {
      if (map_.complete()) return false;  // past end of chapter
      continue;
    }

    LaidOutPage tmp;
    if (!PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), map_.pageStart(target), tmp)) {
      return false;
    }
    ensurePageCacheDir();
    if (!tmp.saveToFile(path, key_, target)) return false;

    if (tmp.atChapterEnd) {
      markMapCompleteIfPlausible(renderer);
    } else {
      const int nextIdx = target + 1;
      if (!map_.hasPage(nextIdx)) {
        map_.pushPageStart(tmp.end);
      } else if (map_.pageStart(nextIdx) != tmp.end) {
        map_.setPageStart(nextIdx, tmp.end);
      }
    }
    LOG_DBG("RVEN", "idle prefetch SAVE p=%d spans=%u fre=%u", target, static_cast<unsigned>(tmp.spans.size()),
            static_cast<unsigned>(ESP.getFreeHeap()));
    return true;  // one page per idle tick
  }
  return false;
}

void RivuletEngine::setRenderKey(const RenderKey& key) {
  // F: map starts are only valid for the render key that produced them.
  if (key_ != key) {
    if (!map_.empty() && map_.renderKey() != key) {
      map_.clear();
    }
    key_ = key;
    map_.setRenderKey(key_);
    laidOutValid_ = false;
    aheadValid_ = false;
    ahead_.clear();
    behindValid_ = false;
    behind_.clear();
    smoothedEstimate_ = 0.0f;
    smoothedAtKnown_ = -1;
  } else {
    key_ = key;
    map_.setRenderKey(key_);
  }
}

void RivuletEngine::seedMapIfEmpty() {
  if (!map_.empty() || chapter_.empty()) return;
  IrCursor start{};
  if (!chapter_.blocks().empty()) start.runIndex = chapter_.blocks()[0].runBegin;
  map_.setRenderKey(key_);
  map_.resetWithStart(start);
}

LayoutParams RivuletEngine::makeParams(const GfxRenderer& renderer) const {
  LayoutParams p;
  p.key = key_;
  p.lineCompression = lineCompression_;
  p.bodyEmPx = std::max(8, renderer.getFontAscenderSize(key_.fontId));
  // Avoid zero/insane line metrics if font lookup fails mid-layout.
  if (p.bodyEmPx < 8) p.bodyEmPx = 14;
  p.extraParagraphSpacing = (key_.flags & 2) != 0;
  // pad[5:4]: 0 = half (legacy bit7 may mean full), 1 = full, 2 = quarter.
  const uint8_t heightFromPad = static_cast<uint8_t>((key_.pad >> 4) & 0x3);
  if (heightFromPad == 1 || heightFromPad == 2) {
    p.extraParagraphSpacingHeight = heightFromPad;
  } else if ((key_.flags & 0x80) != 0) {
    p.extraParagraphSpacingHeight = 1;  // legacy full
  } else {
    p.extraParagraphSpacingHeight = 0;  // half
  }
  p.shouldAbort = mapAbortCheck_;
  return p;
}

LayoutParams RivuletEngine::makeMeasureParams(const GfxRenderer& renderer) const {
  LayoutParams p = makeParams(renderer);
  p.measureOnly = true;
  p.shouldAbort = mapAbortCheck_;
  return p;
}

bool RivuletEngine::ingestHtml(const char* html, const size_t len, const char* irPathSave,
                               const bool armDropCapFirstPara, const uint8_t imageRendering) {
  clear();
  // Cap HTML fed to convert — full novel spines can OOM mid-string growth (device abort).
  constexpr size_t kMaxConvert = 160 * 1024;
  const size_t useLen = len > kMaxConvert ? kMaxConvert : len;
  if (useLen < len) {
    LOG_ERR("RVEN", "HtmlToIr truncating chapter %u -> %u bytes (heap)", static_cast<unsigned>(len),
            static_cast<unsigned>(useLen));
  }
  if (!HtmlToIr::convert(html, useLen, chapter_, armDropCapFirstPara, imageRendering)) {
    LOG_ERR("RVEN", "HtmlToIr failed free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  if (chapter_.failed()) {
    LOG_ERR("RVEN", "HtmlToIr partial OOM blocks=%u text=%u html=%u free=%u maxA=%u — NOT caching",
            static_cast<unsigned>(chapter_.blockCount()), static_cast<unsigned>(chapter_.textSize()),
            static_cast<unsigned>(useLen), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    // Never persist a truncated chapter — that became "13/13" of half a file.
    if (irPathSave && *irPathSave) Storage.remove(irPathSave);
    if (chapter_.empty()) return false;
    // Usable in RAM for this session only; caller must not saveToFile.
  } else if (irPathSave && *irPathSave) {
    if (!chapter_.saveToFile(irPathSave)) {
      LOG_ERR("RVEN", "IR save failed %s", irPathSave);
      // still usable in RAM
    }
  }
  map_.setRenderKey(key_);
  IrCursor start{};
  if (!chapter_.blocks().empty()) {
    start.runIndex = chapter_.blocks()[0].runBegin;
  }
  map_.resetWithStart(start);
  currentPage_ = 0;
  laidOutValid_ = false;
  LOG_DBG("RVEN", "ingest blocks=%u runs=%u text=%u", static_cast<unsigned>(chapter_.blockCount()),
          static_cast<unsigned>(chapter_.runs().size()), static_cast<unsigned>(chapter_.textSize()));
  return true;
}

bool RivuletEngine::loadIr(const char* irPath) {
  clear();
  const ChapterIr::LoadResult st = chapter_.loadFromFileEx(irPath);
  switch (st) {
    case ChapterIr::LoadResult::Ok:
      lastIrLoadResult_ = IrLoadResult::Ok;
      break;
    case ChapterIr::LoadResult::Oom:
      lastIrLoadResult_ = IrLoadResult::Oom;
      return false;
    case ChapterIr::LoadResult::Corrupt:
      lastIrLoadResult_ = IrLoadResult::Corrupt;
      return false;
  }
  map_.setRenderKey(key_);
  IrCursor start{};
  if (!chapter_.blocks().empty()) start.runIndex = chapter_.blocks()[0].runBegin;
  map_.resetWithStart(start);
  currentPage_ = 0;
  laidOutValid_ = false;
  aheadValid_ = false;
  return true;
}

bool RivuletEngine::loadPageMap(const char* mapPath) {
  PageMap m;
  if (!m.loadFromFile(mapPath)) return false;
  if (m.renderKey() != key_) {
    LOG_DBG("RVEN", "page map key mismatch — ignoring");
    return false;
  }
  // Reject maps whose cursors fall outside the loaded IR (stale after reconvert).
  if (!chapter_.empty()) {
    const auto& blocks = chapter_.blocks();
    const size_t nRuns = chapter_.runs().size();
    for (int i = 0; i < m.knownPages(); ++i) {
      const IrCursor c = m.pageStart(i);
      if (c.blockIndex >= blocks.size()) {
        LOG_DBG("RVEN", "page map cursor OOB page=%d block=%u — ignoring", i, static_cast<unsigned>(c.blockIndex));
        return false;
      }
      if (static_cast<size_t>(c.runIndex) > nRuns) {
        LOG_DBG("RVEN", "page map run OOB page=%d run=%u — ignoring", i, static_cast<unsigned>(c.runIndex));
        return false;
      }
      const Block& b = blocks[c.blockIndex];
      const uint32_t runEnd = static_cast<uint32_t>(b.runBegin) + b.runCount;
      // Allow runIndex == runEnd only as empty-block edge; anything past is stale.
      if (b.runCount > 0 && c.runIndex >= runEnd) {
        LOG_DBG("RVEN", "page map run outside block page=%d run=%u blockRuns=[%u+%u) — ignoring", i,
                static_cast<unsigned>(c.runIndex), static_cast<unsigned>(b.runBegin),
                static_cast<unsigned>(b.runCount));
        return false;
      }
    }
  }
  map_ = std::move(m);
  return true;
}

bool RivuletEngine::savePageMap(const char* mapPath) const {
  if (!mapPath || !*mapPath) return false;
  return map_.saveToFile(mapPath);
}

bool RivuletEngine::scrubStaleCompleteMap(const GfxRenderer& renderer) {
  if (!map_.complete() || map_.empty() || chapter_.empty()) return false;
  const int last = map_.knownPages() - 1;
  if (last < 0) {
    map_.markIncomplete();
    return true;
  }
  LaidOutPage tmp;
  if (!PageLayouter::layoutPage(chapter_, renderer, makeMeasureParams(renderer), map_.pageStart(last), tmp)) {
    LOG_DBG("RVEN", "scrubStaleCompleteMap: last page layout fail — incomplete");
    map_.markIncomplete();
    return true;
  }
  // If the recorded last page really ends the IR, trust the map. Do NOT compare
  // against estimatePageCount — that heuristic is padded and was deleting honest
  // 30–40 page maps on PageBack (known*2+1 < est), which made Back a no-op.
  if (tmp.atChapterEnd) {
    const int known = map_.knownPages();
    // Only catch the classic false-complete bug (2–3 page "whole chapter").
    if (known <= 3) {
      const int est =
          chapter_.estimatePageCount(key_.viewportW, key_.viewportH, makeParams(renderer).bodyEmPx, lineCompression_);
      if (est >= 10) {
        LOG_ERR("RVEN", "scrubStaleCompleteMap: tiny complete known=%d est=%d — reset", known, est);
        IrCursor start{};
        if (!chapter_.blocks().empty()) start.runIndex = chapter_.blocks()[0].runBegin;
        map_.resetWithStart(start);
        return true;
      }
    }
    return false;  // honest complete map — keep it
  }

  LOG_DBG("RVEN", "scrubStaleCompleteMap: last page not chapter end (known=%d) — reopening", map_.knownPages());
  map_.markIncomplete();
  if (tmp.end != map_.pageStart(last) && !map_.hasPage(last + 1)) {
    map_.pushPageStart(tmp.end);
  }
  return true;
}

// Mark complete only when live end matches IR size and page count is plausible.
// The caller has already verified atChapterEnd, so this guard exists ONLY to catch
// the classic false-complete bug (a stuck break reporting a 2–3 page novel chapter).
// It must not be strict: estimatePageCount is a padded heuristic, and comparing a
// real page count against it (known*2+1 < est) refused to seal honest maps, which
// left the status bar on "~" forever and forced every PageBack to re-walk.
void RivuletEngine::markMapCompleteIfPlausible(const GfxRenderer& renderer) {
  const int known = map_.knownPages();
  if (known <= 0) return;
  if (known <= 3) {
    const int est =
        chapter_.estimatePageCount(key_.viewportW, key_.viewportH, makeParams(renderer).bodyEmPx, lineCompression_);
    if (est >= 10) {
      LOG_ERR("RVEN", "refuse markComplete known=%d est=%d (implausibly short)", known, est);
      return;
    }
  }
  map_.markComplete(known);
}

bool RivuletEngine::buildPageMap(const GfxRenderer& renderer) {
  if (chapter_.empty()) return false;
  const bool ok = PageLayouter::buildFullPageMap(chapter_, renderer, makeMeasureParams(renderer), map_);
  if (ok) {
    LOG_DBG("RVEN", "page map pages=%d complete=%d", map_.knownPages(), map_.complete() ? 1 : 0);
  }
  return ok;
}

bool RivuletEngine::extendPageMap(const GfxRenderer& renderer, const int maxPages) {
  if (chapter_.empty() || map_.complete() || maxPages <= 0) return false;
  seedMapIfEmpty();
  if (map_.empty()) return false;

  bool progressed = false;
  for (int i = 0; i < maxPages && !map_.complete(); ++i) {
    const int last = map_.knownPages() - 1;
    if (last < 0) break;
    LaidOutPage tmp;
    if (!PageLayouter::layoutPage(chapter_, renderer, makeMeasureParams(renderer), map_.pageStart(last), tmp)) {
      if (tmp.aborted) break;
      // Layout failed mid-map (broken image, empty cell). Skip that block rather
      // than freezing the map here — a stuck map also freezes the page count and
      // makes every later last-page walk fail.
      const IrCursor at = map_.pageStart(last);
      if (!tmp.atChapterEnd && at.blockIndex + 1 < chapter_.blocks().size()) {
        IrCursor skip = at;
        ++skip.blockIndex;
        skip.runIndex = chapter_.blocks()[skip.blockIndex].runBegin;
        skip.byteInRun = 0;
        map_.setPageStart(last, skip);
        progressed = true;
        continue;
      }
      break;
    }
    progressed = true;
    if (tmp.atChapterEnd) {
      markMapCompleteIfPlausible(renderer);
      break;
    }
    if (tmp.end == map_.pageStart(last)) {
      // No forward progress: stuck cursor, not a reliable chapter end. Leave
      // incomplete so nextPage / goToLastPage can still try a live walk.
      LOG_DBG("RVEN", "extendPageMap stuck at page=%d — not marking complete", last);
      break;
    }
    map_.pushPageStart(tmp.end);
    if ((i & 3) == 3) yield();
  }
  return progressed;
}

bool RivuletEngine::ensureMapAhead(const GfxRenderer& renderer, const int aheadPages) {
  if (chapter_.empty()) return false;
  if (map_.complete()) return true;
  seedMapIfEmpty();
  // Need starts[0..current+ahead] ⇒ knownPages >= current + ahead + 1.
  const int want = currentPage_ + std::max(0, aheadPages) + 1;
  const int need = want - map_.knownPages();
  if (need <= 0) return true;
  (void)extendPageMap(renderer, need);
  return map_.complete() || map_.knownPages() >= want;
}

bool RivuletEngine::layoutAtCursor(const GfxRenderer& renderer, const IrCursor& c) {
  laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), c, laidOut_);
  if (laidOutValid_) savePageCache(currentPage_);
  return laidOutValid_;
}

bool RivuletEngine::warmAheadPage(const GfxRenderer& renderer) {
  if (aheadValid_) return false;            // already warm
  if (!laidOutValid_) return false;         // nothing to be ahead OF yet
  if (laidOut_.atChapterEnd) return false;  // no next page in this chapter
  // Same floor as idlePrefetch: loadFromFile reserve() aborts on OOM.
  if (ESP.getMaxAllocHeap() < 16 * 1024 || ESP.getFreeHeap() < 24 * 1024) return false;

  const int nextIdx = currentPage_ + 1;
  char path[220];
  const bool havePath = pageCachePath(nextIdx, path, sizeof(path));
  // Prefer the SD paint cache (same file ensureLaidOut reads). Indexed chapters
  // were still re-layouting every idle warm because this path ignored .rvpg.
  if (havePath) {
    LaidOutPage tmp;
    if (tmp.loadFromFile(path, key_, nextIdx)) {
      const bool startOk = tmp.start == laidOut_.end;
      const bool mapOk = !map_.hasPage(nextIdx) || tmp.start == map_.pageStart(nextIdx);
      if (startOk && mapOk) {
        ahead_ = std::move(tmp);
        aheadValid_ = true;
        return true;
      }
      Storage.remove(path);
    } else if (Storage.exists(path)) {
      // OOM deserialize keeps the file. Idle layout of that same page is how
      // Hangul + warmBehind aborted after first_ink — skip until heap recovers.
      return false;
    }
  }

  aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
  if (!aheadValid_) {
    ahead_.clear();
    return false;
  }
  // Do not SD-save here. Writing .rvpg on idle hitches the loop (PageBack
  // into a mapped chapter then felt frozen). ensureLaidOut persists when shown.
  return true;
}

bool RivuletEngine::warmBehindPage(const GfxRenderer& renderer) {
  if (behindValid_) return false;
  if (!laidOutValid_ || currentPage_ <= 0) return false;
  // Device abort: first_ink of a Hangul page, then idle load of s11_p9_*.rvpg
  // ran vector::reserve with throwing new. Skip until there is a spare block.
  if (ESP.getMaxAllocHeap() < 16 * 1024 || ESP.getFreeHeap() < 24 * 1024) return false;
  // Prefer SD page cache (instant) before re-layout.
  {
    char path[220];
    const int behindIdx = currentPage_ - 1;
    if (pageCachePath(behindIdx, path, sizeof(path))) {
      LaidOutPage tmp;
      if (tmp.loadFromFile(path, key_, behindIdx)) {
        // start must match the map AND end must flow into the page we are on —
        // otherwise page-back shows duplicated or skipped lines.
        const bool startOk = !map_.hasPage(behindIdx) || tmp.start == map_.pageStart(behindIdx);
        const bool endOk = !laidOutValid_ || tmp.end == laidOut_.start;
        if (startOk && endOk) {
          behind_ = std::move(tmp);
          behindValid_ = true;
          return true;
        }
        LOG_DBG("RVEN", "behind cache p=%d rejected (start=%d end=%d)", behindIdx, startOk ? 1 : 0, endOk ? 1 : 0);
        Storage.remove(path);
      } else if (Storage.exists(path)) {
        return false;
      }
    }
  }
  if (!map_.hasPage(currentPage_ - 1)) return false;
  // Full paint layout (not measure-only) — this page may be shown on prevPage.
  behindValid_ =
      PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), map_.pageStart(currentPage_ - 1), behind_);
  if (!behindValid_)
    behind_.clear();
  else if (!pageCacheDir_.empty()) {
    char path[220];
    const int behindIdx = currentPage_ - 1;
    if (pageCachePath(behindIdx, path, sizeof(path))) {
      ensurePageCacheDir();
      (void)behind_.saveToFile(path, key_, behindIdx);
    }
  }
  return behindValid_;
}

bool RivuletEngine::ensureLaidOut(const GfxRenderer& renderer) {
  if (laidOutValid_) return true;
  if (chapter_.empty()) return false;
  if (tryLoadPageCache(currentPage_)) return true;
  IrCursor c{};
  if (map_.hasPage(currentPage_)) {
    c = map_.pageStart(currentPage_);
  } else if (!chapter_.blocks().empty()) {
    c.runIndex = chapter_.blocks()[0].runBegin;
  }
  return layoutAtCursor(renderer, c);
}

bool RivuletEngine::goToStart(const GfxRenderer& renderer) {
  currentPage_ = 0;
  laidOutValid_ = false;
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();
  seedMapIfEmpty();
  if (!ensureLaidOut(renderer)) return false;
  // Warm exactly 1 page ahead for free next-turn (paint path). Deeper map-ahead
  // belongs on idle — layouting kMapAheadPages here blocked open/resume.
  if (!laidOut_.atChapterEnd) {
    if (!map_.hasPage(1)) {
      map_.pushPageStart(laidOut_.end);
    } else if (map_.pageStart(1) != laidOut_.end) {
      map_.setPageStart(1, laidOut_.end);
    }
    // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
  }
  return true;
}

IrCursor RivuletEngine::currentStartCursor() const {
  if (laidOutValid_) return laidOut_.start;
  if (map_.hasPage(currentPage_)) return map_.pageStart(currentPage_);
  return {};
}

bool RivuletEngine::hasCurrentStartCursor() const { return laidOutValid_ || map_.hasPage(currentPage_); }

bool RivuletEngine::resumeAtCursor(const GfxRenderer& renderer, const IrCursor& cursor, const int maxWalkPages) {
  if (chapter_.empty()) return false;
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();

  if (map_.knownPages() > 0) {
    const int page = map_.pageContaining(cursor);
    if (page >= 0 && goToPage(renderer, page, /*maxWalkPages=*/8)) return true;
  }

  seedMapIfEmpty();
  const int budget = maxWalkPages > 0 ? maxWalkPages : 512;
  int walked = 0;
  while (map_.knownPages() > 0) {
    const int last = map_.knownPages() - 1;
    const IrCursor start = map_.pageStart(last);
    LaidOutPage tmp;
    if (!PageLayouter::layoutPage(chapter_, renderer, makeMeasureParams(renderer), start, tmp)) break;
    // This page covers [start, end). Land when the kept cursor sits on it.
    if (!(cursor < start) && (cursor < tmp.end || tmp.atChapterEnd)) {
      currentPage_ = last;
      laidOutValid_ = false;
      return ensureLaidOut(renderer);
    }
    if (tmp.atChapterEnd) {
      currentPage_ = last;
      laidOutValid_ = false;
      return ensureLaidOut(renderer);
    }
    if (tmp.end == start) break;
    if (!map_.hasPage(last + 1)) map_.pushPageStart(tmp.end);
    if (++walked > budget) break;
    if ((walked & 7) == 0) yield();
  }

  currentPage_ = std::max(0, map_.knownPages() - 1);
  laidOutValid_ = false;
  return layoutAtCursor(renderer, cursor);
}

bool RivuletEngine::goToPage(const GfxRenderer& renderer, const int pageIndex, const int maxWalkPages) {
  if (pageIndex < 0) return false;
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();
  if (map_.hasPage(pageIndex)) {
    currentPage_ = pageIndex;
    laidOutValid_ = false;
    // Classic section.bin path: deserialize a finished page when we have one.
    if (tryLoadPageCache(pageIndex)) {
      return true;
    }
    if (!ensureLaidOut(renderer)) return false;
    if (!laidOut_.atChapterEnd) {
      // Live end is ground truth; correct map tail if a loaded .rvpm disagrees.
      if (!map_.hasPage(pageIndex + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(pageIndex + 1) != laidOut_.end) {
        map_.setPageStart(pageIndex + 1, laidOut_.end);
      }
      // One paint-ahead only; idle tick extends the thin map further.
      // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
    }
    return true;
  }
  // Progressive: walk forward from last known page.
  // Cap work so a huge estimate (chapterPageCount without a full map) cannot
  // freeze the UI for minutes layouting an entire novel spine.
  if (map_.empty()) {
    IrCursor start{};
    if (!chapter_.blocks().empty()) start.runIndex = chapter_.blocks()[0].runBegin;
    map_.resetWithStart(start);
  }
  const int walkBudget = maxWalkPages > 0 ? maxWalkPages : 64;
  int walked = 0;
  while (map_.knownPages() <= pageIndex) {
    if (++walked > walkBudget) {
      LOG_ERR("RVEN", "goToPage walk cap target=%d known=%d budget=%d", pageIndex, map_.knownPages(), walkBudget);
      break;
    }
    // Keep the watchdog happy on long walks (even within the cap).
    if ((walked & 7) == 0) {
      yield();
    }
    const int last = map_.knownPages() - 1;
    if (last < 0) break;
    LaidOutPage tmp;
    if (!PageLayouter::layoutPage(chapter_, renderer, makeMeasureParams(renderer), map_.pageStart(last), tmp)) break;
    if (tmp.atChapterEnd) {
      markMapCompleteIfPlausible(renderer);
      break;
    }
    if (tmp.end.blockIndex == map_.pageStart(last).blockIndex && tmp.end.runIndex == map_.pageStart(last).runIndex &&
        tmp.end.byteInRun == map_.pageStart(last).byteInRun) {
      // Stuck mid-walk — stop without poisoning complete.
      break;
    }
    map_.pushPageStart(tmp.end);
  }
  if (!map_.hasPage(pageIndex)) return false;
  currentPage_ = pageIndex;
  laidOutValid_ = false;
  if (!ensureLaidOut(renderer)) return false;
  if (!laidOut_.atChapterEnd) {
    if (!map_.hasPage(pageIndex + 1)) {
      map_.pushPageStart(laidOut_.end);
    } else if (map_.pageStart(pageIndex + 1) != laidOut_.end) {
      map_.setPageStart(pageIndex + 1, laidOut_.end);
    }
    // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
  }
  (void)ensureMapAhead(renderer, kMapAheadPages);
  return true;
}

bool RivuletEngine::nextPage(const GfxRenderer& renderer) {
  if (!ensureLaidOut(renderer)) {
    lastTurnFail_ = TurnFail::LayoutFailed;
    return false;
  }
  if (laidOut_.atChapterEnd) {
    lastTurnFail_ = TurnFail::AtBoundary;
    return false;
  }
  lastTurnFail_ = TurnFail::None;

  // Live layout says more content remains. A "complete" map that claims we are
  // on the last page is stale (false markComplete / old .rvpm) — reopen the map.
  if (map_.complete() && currentPage_ + 1 >= map_.knownTotal()) {
    LOG_DBG("RVEN", "nextPage: map complete=%d but not chapter end — mark incomplete", map_.knownTotal());
    map_.markIncomplete();
  }

  // Authoritative next start = exclusive end of the page we just painted.
  // Never prefer a stale map entry: that skips or overlaps text when re-layout
  // (or a loaded .rvpm) disagrees with the live break.
  const IrCursor nextStart = laidOut_.end;
  // No forward progress (layout stuck) — do not claim chapter end or spin.
  if (nextStart == map_.pageStart(currentPage_) && map_.hasPage(currentPage_)) {
    LOG_DBG("RVEN", "nextPage: no progress at page=%d", currentPage_);
    lastTurnFail_ = TurnFail::LayoutFailed;
    return false;
  }
  if (!map_.hasPage(currentPage_ + 1)) {
    map_.pushPageStart(nextStart);
  } else if (map_.pageStart(currentPage_ + 1) != nextStart) {
    LOG_DBG("RVEN", "nextPage: correct map start for page %d (stale break)", currentPage_ + 1);
    map_.setPageStart(currentPage_ + 1, nextStart);
  }

  ++currentPage_;

  // Fast path: consume pre-warmed next page (saves a full layout on every turn).
  if (aheadValid_ && ahead_.start == nextStart) {
    // Bidirectional: keep the page we leave as behind_ for instant page-back.
    behind_ = std::move(laidOut_);
    behindValid_ = true;
    laidOut_ = std::move(ahead_);
    laidOutValid_ = true;
    aheadValid_ = false;
    ahead_.clear();
    // Record map start for page+1 from this live end; fix if stale.
    if (!laidOut_.atChapterEnd) {
      if (!map_.hasPage(currentPage_ + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(currentPage_ + 1) != laidOut_.end) {
        map_.setPageStart(currentPage_ + 1, laidOut_.end);
      }
      // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
    }
    // Thin map extension is idle-only — keep turn latency to one paint-ahead layout.
    return true;
  }

  // Slow path: layout from the corrected map start (must equal nextStart).
  // Keep the page we leave so page-back is instant (bidirectional index).
  // NOTE: laidOut_ is moved out here. Every failure path below MUST move it back
  // — otherwise a failed forward turn leaves laidOut_ empty and paint() draws a
  // blank content area (paint() early-returns on !laidOutValid_).
  behind_ = std::move(laidOut_);
  behindValid_ = true;
  laidOutValid_ = false;
  aheadValid_ = false;
  if (ensureLaidOut(renderer)) {
    if (!laidOut_.atChapterEnd) {
      if (!map_.hasPage(currentPage_ + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(currentPage_ + 1) != laidOut_.end) {
        map_.setPageStart(currentPage_ + 1, laidOut_.end);
      }
      // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
    }
    return true;
  }
  // Layout of next page failed but cursor advanced — still a successful turn if we
  // can layout from nextStart directly (ensureLaidOut used a stale map entry).
  laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), nextStart, laidOut_);
  if (laidOutValid_) {
    if (!map_.hasPage(currentPage_)) {
      map_.pushPageStart(nextStart);
    } else {
      map_.setPageStart(currentPage_, nextStart);
    }
    if (!laidOut_.atChapterEnd) {
      if (!map_.hasPage(currentPage_ + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(currentPage_ + 1) != laidOut_.end) {
        map_.setPageStart(currentPage_ + 1, laidOut_.end);
      }
      // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
    }
    return true;
  }
  // Both layout attempts failed: restore the page we were on so the reader keeps
  // showing real text instead of a blank frame, and report a transient failure
  // (NOT a chapter boundary — the caller must not advance the spine).
  --currentPage_;
  laidOut_ = std::move(behind_);
  behindValid_ = false;
  behind_.clear();
  laidOutValid_ = true;
  aheadValid_ = false;
  ahead_.clear();
  lastTurnFail_ = TurnFail::LayoutFailed;
  LOG_ERR("RVEN", "nextPage: layout failed at page=%d — restored current page", currentPage_);
  return false;
}

bool RivuletEngine::prevPage(const GfxRenderer& renderer) {
  if (currentPage_ <= 0) {
    lastTurnFail_ = TurnFail::AtBoundary;
    return false;
  }
  lastTurnFail_ = TurnFail::None;
  // Fast path: bidirectional behind_ cache (idle-warmed or left by nextPage).
  if (behindValid_ && currentPage_ >= 1) {
    // ahead_ becomes the page we leave so forward turn-back is also instant.
    ahead_ = std::move(laidOut_);
    aheadValid_ = true;
    laidOut_ = std::move(behind_);
    laidOutValid_ = true;
    behindValid_ = false;
    behind_.clear();
    --currentPage_;
    return true;
  }
  const int fromPage = currentPage_;
  --currentPage_;
  laidOutValid_ = false;
  aheadValid_ = false;  // ahead is only valid for forward direction
  behindValid_ = false;
  behind_.clear();
  if (!ensureLaidOut(renderer)) {
    // Transient failure (heap / bad cursor). Restore the page index so the reader
    // does not think we reached page 0 and jump into the previous chapter.
    currentPage_ = fromPage;
    laidOutValid_ = false;
    lastTurnFail_ = TurnFail::LayoutFailed;
    LOG_ERR("RVEN", "prevPage: layout failed going %d→%d — position restored", fromPage, fromPage - 1);
    return false;
  }
  // Re-layout may produce a different exclusive end than a stale map[next].
  // Keep forward continuity: next map start must equal this page's end.
  if (!laidOut_.atChapterEnd) {
    const int nextIdx = currentPage_ + 1;
    if (!map_.hasPage(nextIdx)) {
      map_.pushPageStart(laidOut_.end);
    } else if (map_.pageStart(nextIdx) != laidOut_.end) {
      // A sealed map came from a verified end-to-end walk. Overwriting one entry
      // truncates the whole tail and drops `complete` (PageMap::setPageStart), so
      // reading backwards through a chapter used to destroy the index we just
      // spent seconds building. Trust the sealed map; only correct while building.
      if (map_.complete()) {
        LOG_DBG("RVEN", "prevPage: page %d end differs from sealed map — keeping map", currentPage_);
      } else {
        LOG_DBG("RVEN", "prevPage: re-break page %d end; truncate stale tail", currentPage_);
        map_.setPageStart(nextIdx, laidOut_.end);
      }
    }
    // Paint-ahead deferred to warmAheadPage() on the idle tick — see below.
  }
  return true;
}

bool RivuletEngine::goToLastPage(const GfxRenderer& renderer, const int maxWalkPages, const bool allowPartial) {
  if (chapter_.empty()) return false;
  // Partial OOM IR ends mid-chapter (DCC: "ripping the T'Ghee totem in two").
  // Refusing is right for forward reading, but for PageBack it meant the reader
  // simply never left the current chapter (silent "nothing happens" refresh).
  if (chapter_.failed() && !allowPartial) {
    LOG_ERR("RVEN", "goToLastPage refuse partial IR text=%u blocks=%u", static_cast<unsigned>(chapter_.textSize()),
            static_cast<unsigned>(chapter_.blockCount()));
    return false;
  }
  if (chapter_.failed()) {
    LOG_ERR("RVEN", "goToLastPage on PARTIAL IR (allowed) text=%u blocks=%u",
            static_cast<unsigned>(chapter_.textSize()), static_cast<unsigned>(chapter_.blockCount()));
  }

  // Instant path only when a complete map's last page is a verified IR end.
  // Never land on "last known mid-map page" — that painted the wrong last line.
  if (map_.complete() && map_.knownTotal() > 0) {
    const int last = map_.knownTotal() - 1;
    if (goToPage(renderer, last, /*maxWalkPages=*/8) && laidOutValid_ && laidOut_.atChapterEnd) {
      LOG_DBG("RVEN", "goToLastPage map-hit page=%d total=%d", currentPage_, map_.knownTotal());
      return true;
    }
    map_.markIncomplete();
  }

  // legacy-style: build the full page map with pure layoutPage walks (not nextPage).
  // nextPage warms paint-ahead and aborts on stuck cursors mid-chapter; a dedicated
  // walk can skip unlayoutable blocks and only succeeds when atChapterEnd is true.
  IrCursor start{};
  if (!chapter_.blocks().empty()) {
    start.runIndex = chapter_.blocks()[0].runBegin;
  }
  map_.setRenderKey(key_);
  map_.resetWithStart(start);
  aheadValid_ = false;
  ahead_.clear();
  behindValid_ = false;
  behind_.clear();

  // The walk below can cover up to `budget` pages (1024 by default) and only
  // ever inspects `page.end` / `page.atChapterEnd`, so it runs measure-only:
  // no GlyphSpan, no per-word string copy, no span vector, per walked page.
  // `paintParams` is used for the pages that actually land in laidOut_.
  const LayoutParams params = makeMeasureParams(renderer);
  const LayoutParams paintParams = makeParams(renderer);
  IrCursor cur = start;
  LaidOutPage page;
  const int budget = maxWalkPages > 0 ? maxWalkPages : 1024;
  int walked = 0;
  // Skip budget must scale with the chapter. A flat 64 stopped a 158-block
  // chapter at block 80 (log: stop=3 block=80/158) even though the walk was
  // still advancing — the reader then landed mid-chapter. One skip per block is
  // the natural bound: the walk can never revisit a block, so it still cannot spin.
  int skips = 0;
  const int kMaxBlockSkips = std::max(64, static_cast<int>(chapter_.blocks().size()) + 8);
  // Telemetry so a user log can say how far the walk actually got and why it
  // stopped — guessing at this from the outside has cost several rounds.
  lastWalkPages_ = 0;
  lastWalkBlock_ = 0;
  lastWalkSkips_ = 0;
  lastWalkStallKind_ = -1;
  lastWalkStop_ = kWalkStopBudget;

  for (int guard = 0; guard < budget; ++guard) {
    if ((guard & 7) == 0) {
      yield();
    }
    page.clear();
    if (!PageLayouter::layoutPage(chapter_, renderer, params, cur, page)) {
      // from already past end → empty chapter / overshot after last content page
      if (page.atChapterEnd) {
        if (walked == 0) {
          // Empty IR body: one empty last page at start.
          currentPage_ = 0;
          laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, paintParams, start, laidOut_);
          if (!laidOutValid_) {
            laidOut_.clear();
            laidOut_.start = start;
            laidOut_.end = start;
            laidOut_.atChapterEnd = true;
            laidOutValid_ = true;
          }
          map_.markComplete(1);
          LOG_INF("RVEN", "goToLastPage empty-chapter page=0");
          return true;
        }
        // Land on previous page (last content).
        const int lastIdx = walked - 1;
        currentPage_ = lastIdx;
        laidOutValid_ = false;
        if (!ensureLaidOut(renderer) || !laidOut_.atChapterEnd) {
          // Re-layout last start explicitly.
          const IrCursor lastStart = map_.pageStart(lastIdx);
          laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, paintParams, lastStart, laidOut_);
        }
        if (laidOutValid_) {
          // We are here because laying out FROM `cur` reported atEnd — the cursor
          // is past the last block, so the page before it IS the last page even if
          // its own atChapterEnd flag came back false (page filled exactly at the
          // final block boundary). Requiring that flag rejected a perfectly good
          // last page and dropped us into the fallback, which is how PageBack kept
          // landing mid-chapter.
          laidOut_.atChapterEnd = true;
          map_.markComplete(map_.knownPages());
          lastWalkPages_ = map_.knownPages();
          lastWalkBlock_ = static_cast<int>(cur.blockIndex);
          lastWalkStop_ = kWalkStopOvershoot;
          LOG_INF("RVEN", "goToLastPage overshot-land page=%d known=%d", currentPage_, map_.knownPages());
          return true;
        }
      }
      // Layout failed and we are NOT at the chapter end: one unlayoutable block
      // (broken image, empty cell, zero-progress spacer) must not end the walk.
      // Aborting here is why PageBack landed on page 1 of a 23 KB chapter — the
      // walk died ~1 page in, so "last page" was never found. Skip the block and
      // keep going, exactly as this function's comment always claimed to do.
      if (cur.blockIndex + 1 < chapter_.blocks().size() && ++skips <= kMaxBlockSkips) {
        IrCursor skip = cur;
        if (lastWalkStallKind_ < 0) {
          // Record what kind of block first refuses to lay out — a long run of
          // these is the real content bug behind the short maps.
          lastWalkStallKind_ = static_cast<int>(chapter_.blocks()[cur.blockIndex].kind);
        }
        ++skip.blockIndex;
        skip.runIndex = chapter_.blocks()[skip.blockIndex].runBegin;
        skip.byteInRun = 0;
        LOG_ERR("RVEN", "goToLastPage layout fail at block %u kind=%d — skipping to %u (page=%d)",
                static_cast<unsigned>(cur.blockIndex), static_cast<int>(chapter_.blocks()[cur.blockIndex].kind),
                static_cast<unsigned>(skip.blockIndex), walked);
        cur = skip;
        lastWalkSkips_ = skips;
        if (!map_.hasPage(walked)) {
          map_.pushPageStart(cur);
        } else {
          map_.setPageStart(walked, cur);
        }
        continue;
      }
      LOG_ERR("RVEN", "goToLastPage layout fail page=%d walked=%d known=%d skips=%d", walked, walked, map_.knownPages(),
              skips);
      lastWalkStop_ = (skips > kMaxBlockSkips) ? kWalkStopSkipsExhausted : kWalkStopLayoutFail;
      break;
    }

    if (page.atChapterEnd) {
      // This page is the true last page of the IR. `page` came from the
      // measure-only walk and therefore carries no spans, so re-run it as a
      // painting pass before handing it to laidOut_ (moving it straight in would
      // land the reader on a blank last page).
      currentPage_ = walked;
      laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, paintParams, page.start, laidOut_);
      if (!laidOutValid_) {
        LOG_ERR("RVEN", "goToLastPage repaint of last page failed page=%d", currentPage_);
        break;
      }
      map_.markComplete(map_.knownPages() > 0 ? map_.knownPages() : walked + 1);
      lastWalkPages_ = map_.knownPages();
      lastWalkBlock_ = static_cast<int>(page.end.blockIndex);
      lastWalkStop_ = kWalkStopReachedEnd;
      LOG_INF("RVEN", "goToLastPage pure-walk page=%d walked=%d known=%d complete=1", currentPage_, walked,
              map_.knownPages());
      return true;
    }

    if (page.end == cur) {
      // Stuck on a block (broken image / empty run). Skip the block so we can
      // still reach the real chapter end — better than stopping mid-chapter.
      if (cur.blockIndex + 1 < chapter_.blocks().size()) {
        IrCursor skip = cur;
        ++skip.blockIndex;
        skip.runIndex = chapter_.blocks()[skip.blockIndex].runBegin;
        skip.byteInRun = 0;
        LOG_DBG("RVEN", "goToLastPage skip stuck block=%u → %u", static_cast<unsigned>(cur.blockIndex),
                static_cast<unsigned>(skip.blockIndex));
        cur = skip;
        // Keep map coherent: next page starts after the skipped block.
        if (!map_.hasPage(walked + 1)) {
          map_.pushPageStart(cur);
        } else {
          map_.setPageStart(walked + 1, cur);
        }
        ++walked;
        continue;
      }
      LOG_ERR("RVEN", "goToLastPage stuck at last block page=%d", walked);
      lastWalkStop_ = kWalkStopStuckLastBlock;
      break;
    }

    // Record start of next page and continue.
    if (!map_.hasPage(walked + 1)) {
      map_.pushPageStart(page.end);
    } else {
      map_.setPageStart(walked + 1, page.end);
    }
    cur = page.end;
    ++walked;
  }

  lastWalkPages_ = map_.knownPages();
  lastWalkBlock_ = static_cast<int>(cur.blockIndex);
  LOG_ERR("RVEN", "goToLastPage incomplete page=%d walked=%d budget=%d known=%d block=%u/%u stop=%u", walked, walked,
          budget, map_.knownPages(), static_cast<unsigned>(cur.blockIndex),
          static_cast<unsigned>(chapter_.blocks().size()), static_cast<unsigned>(lastWalkStop_));
  // Do not paint a mid-chapter page as verified "last" — clear paint state.
  // Keep the walked map so goToBestEffortLastPage can land on known-1.
  // Do NOT reset currentPage_ to 0: a caller that ignores the false return then
  // painted page 0 of this chapter under the previous chapter's status label.
  laidOut_.clear();
  laidOutValid_ = false;
  aheadValid_ = false;
  behindValid_ = false;
  behind_.clear();
  return false;
}

bool RivuletEngine::goToLastPageNearEnd(const GfxRenderer& renderer, const int maxForwardPages,
                                        const bool allowPartial) {
  // Land on REAL last page index with a full page map so next Back is N-1 in-chapter.
  (void)maxForwardPages;
  if (chapter_.empty()) return false;
  if (chapter_.failed() && !allowPartial) return false;

  auto landLast = [&]() -> bool {
    if (!map_.complete() || map_.knownTotal() <= 0) return false;
    const int last = map_.knownTotal() - 1;
    if (!goToPage(renderer, last, /*maxWalkPages=*/32)) return false;
    if (!laidOutValid_) return false;
    // Prefer verified end; still accept last index if paint succeeded (map was sealed).
    return laidOut_.atChapterEnd || last >= 0;
  };

  if (map_.complete() && landLast()) {
    LOG_INF("RVEN", "goToLastPageNearEnd map-hit page=%d total=%d", currentPage_, map_.knownTotal());
    return true;
  }

  if (goToLastPage(renderer, /*maxWalkPages=*/1024, allowPartial)) {
    LOG_INF("RVEN", "goToLastPageNearEnd walk page=%d known=%d", currentPage_, map_.knownPages());
    return true;
  }

  // Stronger rebuild (same as classic section rebuild) then land last.
  LOG_ERR("RVEN", "goToLastPage failed — buildFullPageMap fallback");
  if (buildPageMap(renderer) && landLast()) {
    LOG_INF("RVEN", "goToLastPageNearEnd buildFull page=%d known=%d", currentPage_, map_.knownPages());
    return true;
  }

  return goToBestEffortLastPage(renderer, /*maxWalkPages=*/1024, allowPartial);
}

bool RivuletEngine::goToBestEffortLastPage(const GfxRenderer& renderer, const int maxWalkPages,
                                           const bool allowPartial) {
  if (chapter_.empty()) return false;
  if (goToLastPage(renderer, maxWalkPages, allowPartial)) return true;

  // Incomplete end-walk still filled map starts — land on the deepest known page.
  const int known = map_.knownPages();
  if (known > 1) {
    const int land = known - 1;
    LOG_ERR("RVEN", "goToBestEffortLastPage land known-1=%d known=%d", land, known);
    if (goToPage(renderer, land, std::max(32, land + 16))) return true;
  } else if (known == 1) {
    // Only a page-0 start exists — if that page is already the true end, accept it.
    if (goToPage(renderer, 0, 8) && laidOutValid_ && laidOut_.atChapterEnd) return true;
  }

  const int est = std::max(1, chapterPageCount(&renderer));
  const int guess = std::max(0, est - 1);
  if (guess > 0) {
    LOG_ERR("RVEN", "goToBestEffortLastPage land estimate=%d", guess);
    if (goToPage(renderer, guess, std::max(96, guess + 64))) return true;
  }

  // Do NOT fall back to goToStart. Returning true from page 0 of the previous
  // spine made every PageBack hop chapter→chapter (ch7→ch6 p0→ch5 p0…).
  LOG_ERR("RVEN", "goToBestEffortLastPage failed known=%d est=%d", known, est);
  return false;
}

bool RivuletEngine::tryCompleteMapAtEnd(const GfxRenderer& renderer) {
  if (chapter_.empty() || map_.complete() || map_.empty()) return false;
  const int last = map_.knownPages() - 1;
  if (last < 0) return false;
  LaidOutPage tmp;
  if (!PageLayouter::layoutPage(chapter_, renderer, makeMeasureParams(renderer), map_.pageStart(last), tmp)) {
    return false;
  }
  if (tmp.atChapterEnd) {
    markMapCompleteIfPlausible(renderer);
    return map_.complete();
  }
  if (tmp.end != map_.pageStart(last) && !map_.hasPage(last + 1)) {
    map_.pushPageStart(tmp.end);
    return true;
  }
  return false;
}

bool RivuletEngine::sealMapAtChapterEnd() {
  if (!laidOutValid_ || !laidOut_.atChapterEnd || chapter_.empty() || chapter_.failed()) return false;
  const int total = currentPage_ + 1;
  if (total <= 0) return false;
  // Ensure map has a start for the last page we are on.
  if (!map_.hasPage(currentPage_)) {
    map_.resetWithStart(laidOut_.start);
    // Can't reconstruct full map here — at least mark single-page complete chapters.
    if (currentPage_ == 0) {
      map_.markComplete(1);
      return map_.complete();
    }
    return false;
  }
  map_.markComplete(total);
  LOG_DBG("RVEN", "sealMapAtChapterEnd total=%d", total);
  return map_.complete();
}

int RivuletEngine::chapterPageCount(const GfxRenderer* rendererForEstimate) const {
  // IR estimate is always available (no renderer required for the heuristic).
  const int bodyEm = rendererForEstimate ? std::max(8, rendererForEstimate->getFontAscenderSize(key_.fontId)) : 14;
  const int heuristic = chapter_.estimatePageCount(key_.viewportW, key_.viewportH, bodyEm, lineCompression_);

  const int known = std::max(1, map_.knownPages());
  const int atLeastCurrent = std::max(known, currentPage_ + 1);

  // Live layout reached the real chapter end — do not inflate to a loose estimate
  // (that produced "13/31" after walking only 13 pages of partial IR).
  if (laidOutValid_ && laidOut_.atChapterEnd) {
    smoothedEstimate_ = 0.0f;
    smoothedAtKnown_ = -1;
    return atLeastCurrent;
  }

  // Exact total once fully walked — reject absurdly short "complete" maps.
  if (map_.complete() && map_.knownTotal() > 0) {
    const int total = map_.knownTotal();
    if (total >= atLeastCurrent && !(heuristic >= 6 && total * 2 + 1 < heuristic)) {
      smoothedEstimate_ = 0.0f;
      smoothedAtKnown_ = -1;
      return total;
    }
    // Fall through when complete total is implausibly small.
  }

  // Map-progress extrapolation: once we have a few real page breaks, scale by
  // how far the last cursor has walked through the chapter's blocks. This is
  // the Rivulet analogue of classic Section::estimatedTotalPages (bytesConsumed
  // / totalBytes) — same idea, IR cursors instead of HTML byte offsets.
  int extrapolated = 0;
  if (map_.knownPages() >= 3 && !chapter_.blocks().empty()) {
    const IrCursor& last = map_.pageStart(map_.knownPages() - 1);
    const int blocks = static_cast<int>(chapter_.blocks().size());
    const int reached = std::min(blocks, static_cast<int>(last.blockIndex) + 1);
    if (reached > 0 && reached < blocks) {
      const uint64_t raw =
          (static_cast<uint64_t>(map_.knownPages()) * static_cast<uint64_t>(blocks)) / static_cast<uint64_t>(reached);
      extrapolated = raw > 60000 ? 60000 : static_cast<int>(raw);
    } else if (reached >= blocks) {
      // Cursor is in the last block(s) but map not complete — stay near known.
      extrapolated = map_.knownPages() + 1;
    }
  }

  // Blend: prefer extrapolation when available, else heuristic. Never below the
  // pages already walked / the page the reader is on.
  int raw = heuristic;
  if (extrapolated > 0) {
    // Weight extrapolation higher once we have a decent sample.
    if (map_.knownPages() >= 8) {
      raw = extrapolated;
    } else {
      raw = (extrapolated * 2 + heuristic) / 3;
    }
  }
  raw = std::max(atLeastCurrent, raw);

  // EMA so the status bar does not jump every idle map tick (classic used ALPHA=0.25).
  constexpr float kAlpha = 0.25f;
  if (smoothedEstimate_ <= 0.0f) {
    smoothedEstimate_ = static_cast<float>(raw);
  } else if (map_.knownPages() != smoothedAtKnown_) {
    smoothedEstimate_ += kAlpha * (static_cast<float>(raw) - smoothedEstimate_);
  }
  smoothedAtKnown_ = map_.knownPages();

  const int smoothed = static_cast<int>(smoothedEstimate_ + 0.5f);
  return std::max(atLeastCurrent, smoothed);
}

int RivuletEngine::chapterPageCountForEta(const GfxRenderer* rendererForEstimate) const {
  // Partial IR (idle prefetch, requireCompleteIr=false) sets atChapterEnd when
  // the fragment ends. chapterPageCount then returns the current page, so
  // time-left in chapter stuck on <1m for a 40-page spine.
  if (laidOutValid_ && laidOut_.atChapterEnd && !map_.complete()) {
    const int bodyEm = rendererForEstimate ? std::max(8, rendererForEstimate->getFontAscenderSize(key_.fontId)) : 14;
    const int heuristic = chapter_.estimatePageCount(key_.viewportW, key_.viewportH, bodyEm, lineCompression_);
    const int known = std::max(1, map_.knownPages());
    const int atLeastCurrent = std::max(known, currentPage_ + 1);
    return std::max(atLeastCurrent, heuristic);
  }
  return chapterPageCount(rendererForEstimate);
}

int RivuletEngine::idleMapPagesThisTick(const GfxRenderer* /*rendererForEstimate*/) const {
  if (map_.complete() || chapter_.empty()) return 0;
  // Always one page. A 4–10 page burst is 7–17s of layout with no input sample
  // (v48 freeze). Catch-up is extra idle ticks, not a bigger bite.
  return kIdleMapPagesPerTick;
}

void RivuletEngine::paint(GfxRenderer& renderer, const int originX, const int originY, const bool ahead) const {
  const bool valid = ahead ? aheadValid_ : laidOutValid_;
  if (!valid) return;
  const LaidOutPage& page = ahead ? ahead_ : laidOut_;
  // Focus / guide only when not Book's Style (forced L/C/R/J) — avoids fighting CSS rhythm.
  const bool bookStyle = (key_.flags & 1) != 0;
  const bool focusOn = !bookStyle && (key_.flags & 0x20) != 0;
  const bool guideOn = !bookStyle && (key_.flags & 0x40) != 0;
  static constexpr char kGuideDot[] = "\xc2\xb7";  // U+00B7

  // Thematic breaks first: they sit behind nothing, and drawing them before text
  // keeps the ordering stable if a rule ever shares a line box with a span.
  for (const RulePlate& rule : page.rules) {
    if (rule.w <= 0 || rule.h <= 0) continue;
    renderer.fillRect(originX + rule.x, originY + rule.y, rule.w, rule.h, true);
  }

  for (size_t si = 0; si < page.spans.size(); ++si) {
    const GlyphSpan& sp = page.spans[si];
    if (sp.text.empty()) continue;
    renderer.ensureSdCardFontReady(sp.fontId, sp.text.c_str());
    // GlyphSpan.y is line-box TOP (same contract as classic PageLine / TextBlock).
    // drawText adds ascender for normal ink; DROP_CAP top-aligns N× ink to y.
    const auto style = static_cast<EpdFontFamily::Style>(sp.epdStyle);
    const bool isDrop = (sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::DROP_CAP)) != 0;
    const int nn = isDrop ? (sp.dropScale >= 2 ? static_cast<int>(sp.dropScale) : 2) : 0;
    const int x0 = originX + sp.x;
    const int y0 = originY + sp.y;
    const auto baseStyle = static_cast<EpdFontFamily::Style>(sp.epdStyle & 0x03);

    // Focus Reading (bionic): bold ~45% of each word's codepoints (classic).
    if (focusOn && !isDrop) {
      const char* s = sp.text.c_str();
      int x = x0;
      while (*s) {
        while (*s == ' ' || *s == '\t') {
          x += renderer.getSpaceWidth(sp.fontId, baseStyle);
          ++s;
        }
        if (!*s) break;
        const char* w0 = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        const size_t wlen = static_cast<size_t>(s - w0);
        if (wlen == 0) continue;

        size_t charCount = 0;
        const unsigned char* cp = reinterpret_cast<const unsigned char*>(w0);
        const unsigned char* end = cp + wlen;
        while (cp < end) {
          utf8NextCodepoint(&cp);
          ++charCount;
        }
        constexpr size_t kFocusPct = 45;
        size_t boldChars = (charCount * kFocusPct) / 100;
        if (boldChars < 1) boldChars = 1;
        if (boldChars > 9) boldChars = 9;

        if (boldChars >= charCount) {
          char buf[96];
          const size_t n = wlen < sizeof(buf) - 1 ? wlen : sizeof(buf) - 1;
          std::memcpy(buf, w0, n);
          buf[n] = '\0';
          const auto bold = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD);
          renderer.drawText(sp.fontId, x, y0, buf, true, bold, BidiUtils::BidiBaseDir::LTR, nn);
          x += renderer.getTextAdvanceX(sp.fontId, buf, bold);
        } else {
          const unsigned char* split = reinterpret_cast<const unsigned char*>(w0);
          for (size_t i = 0; i < boldChars; ++i) utf8NextCodepoint(&split);
          const size_t boldBytes = static_cast<size_t>(split - reinterpret_cast<const unsigned char*>(w0));
          char boldBuf[40];
          const size_t bn = boldBytes < sizeof(boldBuf) - 1 ? boldBytes : sizeof(boldBuf) - 1;
          std::memcpy(boldBuf, w0, bn);
          boldBuf[bn] = '\0';
          const auto bold = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD);
          renderer.drawText(sp.fontId, x, y0, boldBuf, true, bold, BidiUtils::BidiBaseDir::LTR, nn);
          const int prefixW = renderer.getTextAdvanceX(sp.fontId, boldBuf, bold);
          char restBuf[96];
          const size_t rn = (wlen - boldBytes) < sizeof(restBuf) - 1 ? (wlen - boldBytes) : sizeof(restBuf) - 1;
          std::memcpy(restBuf, w0 + boldBytes, rn);
          restBuf[rn] = '\0';
          renderer.drawText(sp.fontId, x + prefixW, y0, restBuf, true, baseStyle, BidiUtils::BidiBaseDir::LTR, nn);
          x += prefixW + renderer.getTextAdvanceX(sp.fontId, restBuf, baseStyle);
        }

        // Guide dots: · after word when next word continues on this span or next span same line.
        if (guideOn) {
          bool moreWord = false;
          const char* peek = s;
          while (*peek == ' ' || *peek == '\t') ++peek;
          if (*peek)
            moreWord = true;
          else if (si + 1 < page.spans.size() && page.spans[si + 1].y == sp.y && !page.spans[si + 1].text.empty()) {
            moreWord = true;
          }
          if (moreWord) {
            // Place dot in the gap (half a space before next glyph).
            const int spaceW = renderer.getSpaceWidth(sp.fontId, baseStyle);
            const int dotX = x + std::max(1, spaceW / 4);
            renderer.drawText(sp.fontId, dotX, y0, kGuideDot, true, EpdFontFamily::REGULAR,
                              BidiUtils::BidiBaseDir::LTR);
          }
        }
      }
      continue;
    }

    // Superscript / subscript ride the same baseline offsets the classic engine
    // used (TextBlock::render). drawText scales the glyph and halves the advance
    // for these bits; only the vertical shift is the caller's job.
    const int spanAscender = renderer.getFontAscenderSize(sp.fontId);
    int textY = y0;
    if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::SUP)) != 0) {
      textY -= spanAscender * 2 / 5;
    } else if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::SUB)) != 0) {
      textY += spanAscender / 4;
    }

    renderer.drawText(sp.fontId, x0, textY, sp.text.c_str(), true, style, BidiUtils::BidiBaseDir::LTR, nn);

    // Underline / strikethrough are lines, not glyph bits — drawText does not
    // paint them. Same geometry as the classic TextBlock::render decorations
    // (underline just under the baseline, strike at 4/5 of the ascender).
    if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::TEXT_DECORATION_MASK)) != 0 && !isDrop) {
      int lineW = renderer.getTextAdvanceX(sp.fontId, sp.text.c_str(), style);
      if (lineW > 0) {
        if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::UNDERLINE)) != 0) {
          const int uy = textY + spanAscender + 2;
          renderer.drawLine(x0, uy, x0 + lineW, uy, 2, true);
        }
        if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::STRIKETHROUGH)) != 0) {
          const int sy = textY + spanAscender * 4 / 5;
          renderer.drawLine(x0, sy, x0 + lineW, sy, 2, true);
        }
      }
    }

    // Guide without focus: still place dots between words on the span.
    if (guideOn && !isDrop) {
      const char* s = sp.text.c_str();
      int x = x0;
      while (*s) {
        while (*s == ' ' || *s == '\t') {
          x += renderer.getSpaceWidth(sp.fontId, baseStyle);
          ++s;
        }
        if (!*s) break;
        const char* w0 = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        const size_t wlen = static_cast<size_t>(s - w0);
        if (wlen == 0) continue;
        char buf[96];
        const size_t n = wlen < sizeof(buf) - 1 ? wlen : sizeof(buf) - 1;
        std::memcpy(buf, w0, n);
        buf[n] = '\0';
        x += renderer.getTextAdvanceX(sp.fontId, buf, baseStyle);
        bool moreWord = false;
        const char* peek = s;
        while (*peek == ' ' || *peek == '\t') ++peek;
        if (*peek)
          moreWord = true;
        else if (si + 1 < page.spans.size() && page.spans[si + 1].y == sp.y && !page.spans[si + 1].text.empty()) {
          moreWord = true;
        }
        if (moreWord) {
          const int spaceW = renderer.getSpaceWidth(sp.fontId, baseStyle);
          renderer.drawText(sp.fontId, x + std::max(1, spaceW / 4), y0, kGuideDot, true, EpdFontFamily::REGULAR,
                            BidiUtils::BidiBaseDir::LTR);
        }
      }
    }
  }
  // Image plates: painted by the reader activity (needs Epub extract + SD cache paths).
}

}  // namespace rivulet
