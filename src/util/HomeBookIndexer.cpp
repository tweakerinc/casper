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
#include "util/CasperBookStore.h"
#include "util/CasperPaths.h"
#include "util/SystemLog.h"

// Heap floors. Indexing is strictly optional work, so it gets out of the way well
// before anything the user asked for would be starved.
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
  finished_ = false;
  openFailed_ = false;
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

bool HomeBookIndexer::step(GfxRenderer& renderer) {
  if (finished_ || bookPath_.empty() || openFailed_) return false;
  if (ESP.getFreeHeap() < kMinFreeHeap || ESP.getMaxAllocHeap() < kMinMaxAlloc) return false;
  if (!ensureOpen()) return false;

  const int spineCount = epub_->getSpineItemsCount();
  const unsigned imgMode = static_cast<unsigned>(SETTINGS.imageRendering);

  // Find the next spine that still needs a map. Skipping is cheap (one exists()
  // per spine), so a mostly-indexed book costs almost nothing per pass.
  int target = -1;
  while (nextSpine_ < spineCount) {
    const int candidate = nextSpine_++;
    if (epub_->getSpineItem(candidate).href.empty()) continue;
    char mapPath[200];
    std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir_.c_str(), candidate, imgMode);
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

  const uint32_t t0 = millis();
  rivulet::RivuletEngine& eng = engine_->engine;

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

  chapterload::Hooks hooks;
  hooks.ctx = &eng;
  hooks.prepareHeap = [](void* ctx, bool) {
    // No reader state to protect here; just drop the previous chapter.
    static_cast<rivulet::RivuletEngine*>(ctx)->clear();
    yield();
  };
  hooks.prepareImages = nullptr;

  const chapterload::Result loaded = chapterload::loadChapterIr(req, hooks);
  if (!loaded.ok) {
    LOG_ERR("HIDX", "spine %d load failed", target);
    return true;  // consumed a slot; try the next spine on the following pass
  }

  // A partial convert would produce a map for a truncated chapter — worse than
  // no map, because it would be trusted later. Leave it for the reader to redo
  // when more heap is free.
  if (loaded.partial) {
    LOG_ERR("HIDX", "spine %d partial IR — not mapping", target);
    eng.clear();
    return true;
  }

  const bool built = eng.buildPageMap(renderer) && eng.mapComplete();
  if (built) {
    char mapPath[200];
    std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir_.c_str(), target, imgMode);
    if (eng.savePageMap(mapPath)) {
      ++indexed_;
      LOG_INF("HIDX", "spine %d mapped pages=%d in %lums", target, eng.mapKnownPages(),
              static_cast<unsigned long>(millis() - t0));
      SystemLog::logTiming("HIDX", "spine=%d pages=%d ms=%lu fre=%u", target, eng.mapKnownPages(),
                           static_cast<unsigned long>(millis() - t0),
                           static_cast<unsigned>(ESP.getFreeHeap()));
    }
  } else {
    LOG_ERR("HIDX", "spine %d map incomplete known=%d", target, eng.mapKnownPages());
  }

  // Drop the chapter so Home is not sitting on a chapter's worth of IR between
  // passes — the next step reloads from the .rvir it just wrote.
  eng.clear();
  return true;
}
