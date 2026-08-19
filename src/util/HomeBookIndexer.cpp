#include "HomeBookIndexer.h"

#include <Epub.h>
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <RivuletEngine.h>

#include <cstdio>

#include "CasperSettings.h"
#include "activities/reader/ChapterLoader.h"
#include "activities/reader/ReaderRenderKey.h"
#include "util/CasperBookStore.h"
#include "util/CasperPaths.h"
#include "util/SystemLog.h"

// Heap floors. Indexing is strictly optional work, so it gets out of the way well
// before anything the user asked for would be starved.
//
// kMinMaxAlloc is higher than it would need to be with a framebuffer loan,
// because this path deliberately refuses the loan: Home's painted pixels live in
// that buffer and the loan hands it back white (see ChapterLoader Request::
// lendFrameBuffer). A chapter that will not convert within the heap we already
// have is simply left for the reader.
namespace {
constexpr uint32_t kMinFreeHeap = 90 * 1024;
constexpr uint32_t kMinMaxAlloc = 56 * 1024;
}  // namespace

struct HomeBookIndexer::Engine {
  rivulet::RivuletEngine engine;
};

HomeBookIndexer::HomeBookIndexer() = default;
HomeBookIndexer::~HomeBookIndexer() = default;

void HomeBookIndexer::begin(const std::string& bookPath) {
  if (bookPath == bookPath_) return;  // already targeting this book
  reset();
  bookPath_ = bookPath;
  finished_ = bookPath.empty();
}

void HomeBookIndexer::reset() {
  epub_.reset();
  engine_.reset();
  bookPath_.clear();
  irDir_.clear();
  nextSpine_ = 0;
  indexed_ = 0;
  activeSpine_ = -1;
  burstsThisChapter_ = 0;
  chapterStartMs_ = 0;
  finished_ = false;
  openFailed_ = false;
}

void HomeBookIndexer::mapPathFor(const int spine, char* out, const size_t outSize) const {
  std::snprintf(out, outSize, "%s/s%d_m%u.rvpm", irDir_.c_str(), spine,
                static_cast<unsigned>(SETTINGS.imageRendering));
}

bool HomeBookIndexer::ensureOpen() {
  if (openFailed_) return false;
  if (epub_ && engine_) return true;
  if (bookPath_.empty()) return false;

  if (!Storage.exists(bookPath_.c_str())) {
    openFailed_ = true;
    return false;
  }

  irDir_ = CasperBook::rivuletDirForPath(bookPath_);
  if (irDir_.empty()) {
    openFailed_ = true;
    return false;
  }

  auto epub = makeUniqueNoThrow<Epub>(bookPath_, CasperPaths::kPackageCacheRoot);
  if (!epub) {
    LOG_ERR("HIDX", "OOM allocating Epub");
    openFailed_ = true;
    return false;
  }
  // skipLoadingCss: Rivulet builds IR from tags, not publisher CSS.
  if (!epub->load(true, /*skipLoadingCss=*/true)) {
    LOG_ERR("HIDX", "epub load failed %s", bookPath_.c_str());
    openFailed_ = true;
    return false;
  }

  auto eng = makeUniqueNoThrow<Engine>();
  if (!eng) {
    LOG_ERR("HIDX", "OOM allocating engine");
    openFailed_ = true;
    return false;
  }

  epub_ = std::move(epub);
  engine_ = std::move(eng);
  LOG_INF("HIDX", "opened %s spines=%d", bookPath_.c_str(), epub_->getSpineItemsCount());
  return true;
}

bool HomeBookIndexer::beginNextChapter(GfxRenderer& renderer) {
  const int spineCount = epub_->getSpineItemsCount();

  // Find the next spine that still needs a map. Skipping is cheap (one exists()
  // per spine), so a mostly-indexed book costs almost nothing per pass.
  int target = -1;
  while (nextSpine_ < spineCount) {
    const int candidate = nextSpine_++;
    if (epub_->getSpineItem(candidate).href.empty()) continue;
    char mapPath[200];
    mapPathFor(candidate, mapPath, sizeof(mapPath));
    if (Storage.exists(mapPath)) continue;
    target = candidate;
    break;
  }

  if (target < 0) {
    finished_ = true;
    LOG_INF("HIDX", "book fully indexed: %s (%d chapters this pass)", bookPath_.c_str(), indexed_);
    SystemLog::logTiming("HIDX", "complete book=%s indexed=%d", bookPath_.c_str(), indexed_);
    // Nothing more to do — give the heap back immediately.
    epub_.reset();
    engine_.reset();
    return false;
  }

  rivulet::RivuletEngine& eng = engine_->engine;

  // The map is only usable if it is built under the exact key the reader will
  // present on load; anything else is silently rejected by loadPageMap and the
  // stale .rvpm then makes this indexer skip the chapter forever.
  const readerkey::Layout layout = readerkey::compute(renderer);
  eng.setRenderKey(layout.key);
  eng.setLineCompression(layout.lineCompression);

  chapterload::Request req;
  req.epub = epub_.get();
  req.engine = &eng;
  req.renderer = &renderer;
  req.irDir = irDir_;
  req.spineIndex = target;
  req.imageRendering = SETTINGS.imageRendering;
  req.requireCompleteIr = false;
  // Never paints, so no laid-out page cache and no image dimension probing.
  req.bindPageCache = false;
  // Home is still on the panel: taking the framebuffer would blank it.
  req.lendFrameBuffer = false;

  chapterload::Hooks hooks;
  hooks.ctx = &eng;
  hooks.prepareHeap = [](void* ctx, bool) {
    // No reader state to protect here; just drop the previous chapter.
    static_cast<rivulet::RivuletEngine*>(ctx)->clear();
    yield();
  };
  hooks.prepareImages = nullptr;

  const uint32_t t0 = millis();
  const chapterload::Result loaded = chapterload::loadChapterIr(req, hooks);
  const uint32_t loadMs = millis() - t0;

  if (!loaded.ok) {
    LOG_ERR("HIDX", "spine %d load failed in %lums", target, static_cast<unsigned long>(loadMs));
    SystemLog::logTiming("HIDX", "spine=%d load_fail ms=%lu", target, static_cast<unsigned long>(loadMs));
    eng.clear();
    return true;  // consumed a slot; try the next spine on the following pass
  }

  // A partial convert would produce a map for a truncated chapter — worse than
  // no map, because it would be trusted later. Leave it for the reader to redo
  // when more heap is free.
  if (loaded.partial) {
    LOG_ERR("HIDX", "spine %d partial IR — not mapping", target);
    SystemLog::logTiming("HIDX", "spine=%d partial ms=%lu", target, static_cast<unsigned long>(loadMs));
    eng.clear();
    return true;
  }

  activeSpine_ = target;
  burstsThisChapter_ = 0;
  chapterStartMs_ = millis();
  SystemLog::logTiming("HIDX", "spine=%d loaded cache=%d ms=%lu fre=%u", target, loaded.fromCache ? 1 : 0,
                       static_cast<unsigned long>(loadMs), static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

void HomeBookIndexer::measureBurst(GfxRenderer& renderer) {
  rivulet::RivuletEngine& eng = engine_->engine;

  // extendPageMap measures at most kPagesPerStep pages and yields as it goes, so
  // control returns to the Home loop in tens of ms rather than tens of seconds.
  const bool progressed = eng.extendPageMap(renderer, kPagesPerStep);
  ++burstsThisChapter_;

  if (eng.mapComplete()) {
    finishChapter(/*mapped=*/true);
    return;
  }
  if (!progressed) {
    // Stuck cursor or an unlayoutable tail: the map will never complete, and
    // saving a short one would have the reader trust a wrong page count.
    LOG_ERR("HIDX", "spine %d stalled at %d pages — abandoning", activeSpine_, eng.mapKnownPages());
    SystemLog::logTiming("HIDX", "spine=%d stalled pages=%d", activeSpine_, eng.mapKnownPages());
    finishChapter(/*mapped=*/false);
    return;
  }
  if (burstsThisChapter_ >= kMaxBurstsPerChapter) {
    LOG_ERR("HIDX", "spine %d exceeded burst budget at %d pages", activeSpine_, eng.mapKnownPages());
    finishChapter(/*mapped=*/false);
  }
}

void HomeBookIndexer::finishChapter(const bool mapped) {
  rivulet::RivuletEngine& eng = engine_->engine;
  const int spine = activeSpine_;

  if (mapped) {
    char mapPath[200];
    mapPathFor(spine, mapPath, sizeof(mapPath));
    if (eng.savePageMap(mapPath)) {
      ++indexed_;
      const unsigned long ms = static_cast<unsigned long>(millis() - chapterStartMs_);
      LOG_INF("HIDX", "spine %d mapped pages=%d in %lums", spine, eng.mapKnownPages(), ms);
      SystemLog::logTiming("HIDX", "spine=%d pages=%d ms=%lu bursts=%d fre=%u", spine, eng.mapKnownPages(), ms,
                           burstsThisChapter_, static_cast<unsigned>(ESP.getFreeHeap()));
    } else {
      LOG_ERR("HIDX", "spine %d map save failed", spine);
    }
  }

  // Drop the chapter so Home is not sitting on a chapter's worth of IR between
  // chapters — the next one reloads from the .rvir it just wrote.
  eng.clear();
  activeSpine_ = -1;
  burstsThisChapter_ = 0;
}

bool HomeBookIndexer::step(GfxRenderer& renderer) {
  if (finished_ || bookPath_.empty() || openFailed_) return false;
  if (ESP.getFreeHeap() < kMinFreeHeap || ESP.getMaxAllocHeap() < kMinMaxAlloc) {
    // Heap got tight mid-chapter (a cover decode, a menu). Release rather than
    // hold a chapter's IR hostage while nothing can progress.
    if (activeSpine_ >= 0) finishChapter(/*mapped=*/false);
    return false;
  }
  if (!ensureOpen()) return false;

  if (activeSpine_ < 0) return beginNextChapter(renderer);

  measureBurst(renderer);
  return true;
}
