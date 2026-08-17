#include "RivuletEngine.h"

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "FontLadder.h"
#include "HtmlToIr.h"

namespace rivulet {

void RivuletEngine::clear() {
  chapter_.clear();
  map_.clear();
  laidOut_.clear();
  ahead_.clear();
  currentPage_ = 0;
  laidOutValid_ = false;
  aheadValid_ = false;
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
  if (!chapter_.loadFromFile(irPath)) return false;
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
        LOG_DBG("RVEN", "page map cursor OOB page=%d block=%u — ignoring", i,
                static_cast<unsigned>(c.blockIndex));
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
  if (!PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), map_.pageStart(last), tmp)) {
    LOG_DBG("RVEN", "scrubStaleCompleteMap: last page layout fail — incomplete");
    map_.markIncomplete();
    return true;
  }
  const int est = chapter_.estimatePageCount(key_.viewportW, key_.viewportH, makeParams(renderer).bodyEmPx,
                                             lineCompression_);
  // Last page ends chapter, but page count is absurdly low vs IR size (false
  // complete from stuck layout / old end==start bug). Discard and rebuild.
  if (tmp.atChapterEnd) {
    if (est >= 6 && map_.knownPages() * 2 + 1 < est) {
      LOG_ERR("RVEN", "scrubStaleCompleteMap: complete too short known=%d est=%d — reset",
              map_.knownPages(), est);
      IrCursor start{};
      if (!chapter_.blocks().empty()) start.runIndex = chapter_.blocks()[0].runBegin;
      map_.resetWithStart(start);
      return true;
    }
    return false;  // complete flag looks honest
  }

  LOG_DBG("RVEN", "scrubStaleCompleteMap: last page not chapter end (known=%d) — reopening",
          map_.knownPages());
  map_.markIncomplete();
  // Seed continuation so idle/next can extend from the true end of the last start.
  if (tmp.end != map_.pageStart(last) && !map_.hasPage(last + 1)) {
    map_.pushPageStart(tmp.end);
  }
  return true;
}

// Mark complete only when live end matches IR size and page count is plausible.
void RivuletEngine::markMapCompleteIfPlausible(const GfxRenderer& renderer) {
  const int known = map_.knownPages();
  if (known <= 0) return;
  const int est = chapter_.estimatePageCount(key_.viewportW, key_.viewportH, makeParams(renderer).bodyEmPx,
                                             lineCompression_);
  if (est >= 6 && known * 2 + 1 < est) {
    LOG_ERR("RVEN", "refuse markComplete known=%d est=%d (too short)", known, est);
    return;
  }
  map_.markComplete(known);
}

bool RivuletEngine::buildPageMap(const GfxRenderer& renderer) {
  if (chapter_.empty()) return false;
  const bool ok = PageLayouter::buildFullPageMap(chapter_, renderer, makeParams(renderer), map_);
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
    if (!PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), map_.pageStart(last), tmp)) {
      // Layout failed mid-map (e.g. empty image-only transient). Do NOT mark
      // complete — that froze chapters at 2–3 pages after a stuck break.
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
  return laidOutValid_;
}

bool RivuletEngine::ensureLaidOut(const GfxRenderer& renderer) {
  if (laidOutValid_) return true;
  if (chapter_.empty()) return false;
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
    aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
  }
  return true;
}

bool RivuletEngine::goToPage(const GfxRenderer& renderer, const int pageIndex, const int maxWalkPages) {
  if (pageIndex < 0) return false;
  aheadValid_ = false;
  if (map_.hasPage(pageIndex)) {
    currentPage_ = pageIndex;
    laidOutValid_ = false;
    if (!ensureLaidOut(renderer)) return false;
    if (!laidOut_.atChapterEnd) {
      // Live end is ground truth; correct map tail if a loaded .rvpm disagrees.
      if (!map_.hasPage(pageIndex + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(pageIndex + 1) != laidOut_.end) {
        map_.setPageStart(pageIndex + 1, laidOut_.end);
      }
      // One paint-ahead only; idle tick extends the thin map further.
      aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
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
    if (!PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), map_.pageStart(last), tmp)) break;
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
    aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
  }
  (void)ensureMapAhead(renderer, kMapAheadPages);
  return true;
}

bool RivuletEngine::nextPage(const GfxRenderer& renderer) {
  if (!ensureLaidOut(renderer)) return false;
  if (laidOut_.atChapterEnd) return false;

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
      aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
    }
    // Thin map extension is idle-only — keep turn latency to one paint-ahead layout.
    return true;
  }

  // Slow path: layout from the corrected map start (must equal nextStart).
  laidOutValid_ = false;
  aheadValid_ = false;
  if (ensureLaidOut(renderer)) {
    if (!laidOut_.atChapterEnd) {
      if (!map_.hasPage(currentPage_ + 1)) {
        map_.pushPageStart(laidOut_.end);
      } else if (map_.pageStart(currentPage_ + 1) != laidOut_.end) {
        map_.setPageStart(currentPage_ + 1, laidOut_.end);
      }
      aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
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
      aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
    }
    return true;
  }
  --currentPage_;
  return false;
}

bool RivuletEngine::prevPage(const GfxRenderer& renderer) {
  if (currentPage_ <= 0) return false;
  --currentPage_;
  laidOutValid_ = false;
  aheadValid_ = false;  // ahead is only valid for forward direction
  if (!ensureLaidOut(renderer)) return false;
  // Re-layout may produce a different exclusive end than a stale map[next].
  // Keep forward continuity: next map start must equal this page's end.
  if (!laidOut_.atChapterEnd) {
    const int nextIdx = currentPage_ + 1;
    if (!map_.hasPage(nextIdx)) {
      map_.pushPageStart(laidOut_.end);
    } else if (map_.pageStart(nextIdx) != laidOut_.end) {
      LOG_DBG("RVEN", "prevPage: re-break page %d end; truncate stale tail", currentPage_);
      map_.setPageStart(nextIdx, laidOut_.end);
    }
    aheadValid_ = PageLayouter::layoutPage(chapter_, renderer, makeParams(renderer), laidOut_.end, ahead_);
  }
  return true;
}

bool RivuletEngine::goToLastPage(const GfxRenderer& renderer, const int maxWalkPages) {
  if (chapter_.empty()) return false;
  // Partial OOM IR ends mid-chapter (DCC: "ripping the T'Ghee totem in two").
  // Caller must require a full convert for prev-chapter; refuse false ends here.
  if (chapter_.failed()) {
    LOG_ERR("RVEN", "goToLastPage refuse partial IR text=%u blocks=%u",
            static_cast<unsigned>(chapter_.textSize()), static_cast<unsigned>(chapter_.blockCount()));
    return false;
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

  const LayoutParams params = makeParams(renderer);
  IrCursor cur = start;
  LaidOutPage page;
  const int budget = maxWalkPages > 0 ? maxWalkPages : 1024;
  int walked = 0;

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
          laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, params, start, laidOut_);
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
          laidOutValid_ = PageLayouter::layoutPage(chapter_, renderer, params, lastStart, laidOut_);
        }
        if (laidOutValid_ && laidOut_.atChapterEnd) {
          map_.markComplete(map_.knownPages());
          LOG_INF("RVEN", "goToLastPage overshot-land page=%d known=%d", currentPage_, map_.knownPages());
          return true;
        }
      }
      LOG_ERR("RVEN", "goToLastPage layout fail page=%d walked=%d known=%d", walked, walked, map_.knownPages());
      break;
    }

    if (page.atChapterEnd) {
      // This page is the true last page of the IR.
      currentPage_ = walked;
      laidOut_ = std::move(page);
      laidOutValid_ = true;
      map_.markComplete(map_.knownPages() > 0 ? map_.knownPages() : walked + 1);
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

  LOG_ERR("RVEN", "goToLastPage incomplete page=%d walked=%d budget=%d known=%d (not chapter end)", walked, walked,
          budget, map_.knownPages());
  // Do not paint a mid-chapter page as "last" — invalidate paint state so caller
  // rejects the land (turnPrev restores origin spine).
  laidOut_.clear();
  laidOutValid_ = false;
  aheadValid_ = false;
  currentPage_ = 0;
  return false;
}

int RivuletEngine::chapterPageCount(const GfxRenderer* rendererForEstimate) const {
  // IR estimate is always available (no renderer required for the heuristic).
  const int bodyEm =
      rendererForEstimate ? std::max(8, rendererForEstimate->getFontAscenderSize(key_.fontId)) : 14;
  const int estimate =
      chapter_.estimatePageCount(key_.viewportW, key_.viewportH, bodyEm, lineCompression_);

  const int known = std::max(1, map_.knownPages());
  const int atLeastCurrent = std::max(known, currentPage_ + 1);

  // Live layout reached the real chapter end — do not inflate to a loose estimate
  // (that produced "13/31" after walking only 13 pages of partial IR).
  if (laidOutValid_ && laidOut_.atChapterEnd) {
    return atLeastCurrent;
  }

  // Exact total once fully walked — reject absurdly short "complete" maps.
  if (map_.complete() && map_.knownTotal() > 0) {
    const int total = map_.knownTotal();
    if (total >= atLeastCurrent && !(estimate >= 6 && total * 2 + 1 < estimate)) {
      return total;
    }
    // Fall through to estimate when complete total is implausibly small.
  }

  // knownPages is only a lower bound while incomplete — never *replace* the estimate
  // with it (that produced "2/3" / "3/3" mid-chapter in the status bar / menu).
  if (estimate > 0) return std::max(atLeastCurrent, estimate);
  return atLeastCurrent;
}

void RivuletEngine::paint(GfxRenderer& renderer, const int originX, const int originY) const {
  if (!laidOutValid_) return;
  // Focus / guide only when not Book's Style (forced L/C/R/J) — avoids fighting CSS rhythm.
  const bool bookStyle = (key_.flags & 1) != 0;
  const bool focusOn = !bookStyle && (key_.flags & 0x20) != 0;
  const bool guideOn = !bookStyle && (key_.flags & 0x40) != 0;
  static constexpr char kGuideDot[] = "\xc2\xb7";  // U+00B7

  for (size_t si = 0; si < laidOut_.spans.size(); ++si) {
    const GlyphSpan& sp = laidOut_.spans[si];
    if (sp.text.empty()) continue;
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
          if (*peek) moreWord = true;
          else if (si + 1 < laidOut_.spans.size() && laidOut_.spans[si + 1].y == sp.y &&
                   !laidOut_.spans[si + 1].text.empty()) {
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

    renderer.drawText(sp.fontId, x0, y0, sp.text.c_str(), true, style, BidiUtils::BidiBaseDir::LTR, nn);

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
        if (*peek) moreWord = true;
        else if (si + 1 < laidOut_.spans.size() && laidOut_.spans[si + 1].y == sp.y &&
                 !laidOut_.spans[si + 1].text.empty()) {
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
