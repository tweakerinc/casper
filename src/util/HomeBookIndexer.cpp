#include "HomeBookIndexer.h"

#include <Epub.h>
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <RivuletEngine.h>
#include <SourceIdentity.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "activities/reader/ChapterLoader.h"
#include "activities/reader/ChapterGeometry.h"
#include "activities/reader/ReaderRenderKey.h"
#include "util/CrossPointBookStore.h"
#include "util/CrossPointPaths.h"
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
  std::unique_ptr<chapterload::Session> load;
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
  if(engine_ && activeSpine_>=0)finishChapter(false);
  engine_.reset();
  epub_.reset();
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
  std::snprintf(out, outSize, "%s/s%d_m%u.rvpm", irDir_.c_str(), spine, static_cast<unsigned>(SETTINGS.imageRendering));
}

bool HomeBookIndexer::ensureOpen() {
  if (openFailed_) return false;
  if (epub_ && engine_) return true;
  if (bookPath_.empty()) return false;

  if (!Storage.exists(bookPath_.c_str())) {
    openFailed_ = true;
    return false;
  }

  irDir_ = rivulet::sourceCacheDirectory(CrossPointBook::rivuletDirForPath(bookPath_),bookPath_);
  if (irDir_.empty()) {
    openFailed_ = true;
    return false;
  }

  auto epub = makeUniqueNoThrow<Epub>(bookPath_, CrossPointPaths::kPackageCacheRoot);
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
  const readerkey::Layout layout = readerkey::compute(renderer);

  // Find the next spine that still needs a map. Skipping is cheap (one exists()
  // per spine), so a mostly-indexed book costs almost nothing per pass.
  int target = -1;
  int probes=0;
  while (nextSpine_ < spineCount && ++probes<=8) {
    const int candidate = nextSpine_++;
    if (epub_->getSpineItem(candidate).href.empty()) continue;
    char mapPath[200];
    mapPathFor(candidate, mapPath, sizeof(mapPath));
    rivulet::PageMap map;
    if (map.loadFromFile(mapPath) && map.complete() && map.renderKey()==layout.key) continue;
    target = candidate;
    break;
  }

  if(target<0 && nextSpine_<spineCount)return true;
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
  // Same image geometry as the reader; no pixels or laid-out paint cache.
  req.bindPageCache = false;
  // Home is still on the panel: taking the framebuffer would blank it.
  req.lendFrameBuffer = false;

  engine_->load=makeUniqueNoThrow<chapterload::Session>(req);
  if(!engine_->load){eng.clear();return true;}

  activeSpine_ = target;
  burstsThisChapter_ = 0;
  chapterStartMs_ = millis();
  SystemLog::logTiming("HIDX", "spine=%d prepare queued", target);
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

}

void HomeBookIndexer::finishChapter(const bool mapped) {
  engine_->load.reset();
  rivulet::RivuletEngine& eng = engine_->engine;
  const int spine = activeSpine_;

  if(!mapped && !eng.chapter().failed() && eng.mapKnownPages()>0){
    char partial[224];std::snprintf(partial,sizeof(partial),"%s/s%d_m%u.rvpm.part",irDir_.c_str(),spine,unsigned(SETTINGS.imageRendering));
    (void)eng.savePageMap(partial);
  }
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
  if(engine_->load){
    const auto status=engine_->load->step();
    if(status==chapterload::Session::Status::Working)return true;
    const bool ok=status==chapterload::Session::Status::Done&&engine_->load->result().ok;
    engine_->load.reset();
    if(!ok)finishChapter(false);
    return true;
  }
  measureBurst(renderer);
  return true;
}
