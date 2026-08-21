#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <optional>

#include "CasperSettings.h"
#include "Epub.h"
#include "RivuletReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "util/BookCacheUtils.h"
#include "util/CasperBookStore.h"
#include "util/CasperPaths.h"
#include "util/QrTimingLog.h"
#include "util/SystemLog.h"

bool ReaderActivity::s_preferFastFirstRefresh = false;
bool ReaderActivity::s_deferFirstPageTextAa = false;
uint32_t ReaderActivity::s_openWallStartMs = 0;

void ReaderActivity::setOpenHints(const bool preferFastFirstRefresh, const bool deferFirstPageTextAa) {
  s_preferFastFirstRefresh = preferFastFirstRefresh;
  s_deferFirstPageTextAa = deferFirstPageTextAa;
  s_openWallStartMs = millis();
}

bool ReaderActivity::hasOpenHints() {
  return s_preferFastFirstRefresh || s_deferFirstPageTextAa || s_openWallStartMs != 0;
}

bool ReaderActivity::takeOpenHints(bool& preferFastFirstRefresh, bool& deferFirstPageTextAa,
                                   uint32_t& openWallStartMs) {
  preferFastFirstRefresh = s_preferFastFirstRefresh;
  deferFirstPageTextAa = s_deferFirstPageTextAa;
  openWallStartMs = s_openWallStartMs;
  const bool had = s_preferFastFirstRefresh || s_deferFirstPageTextAa || s_openWallStartMs != 0;
  s_preferFastFirstRefresh = false;
  s_deferFirstPageTextAa = false;
  // Keep s_openWallStartMs until first-ink log (EpubReader may read openWallStartMs()).
  return had;
}

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  // Step timing: on a warm open the gap between "open start" and the EPUB line
  // was ~3.2s while epub->load itself reported 132ms, so the cost is in the
  // pre-load SD work. Log each step so it is measured, not guessed at.
  const uint32_t tStep0 = millis();
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }
  const uint32_t tExists = millis();

  // Live layout is CrossPoint/CrossInk `epub_<hash>` (same as Epub::getCachePath).
  // The book_<id>/package import was abandoned — do not probe that tree.
  // Warm HIT: book.bin already on disk, so skip mkdir (three ~120ms scans).
  const std::string bookBin = CasperBook::packageDirForPath(path) + "/book.bin";
  if (!Storage.exists(bookBin.c_str())) {
    (void)CasperBook::openBook(path, "", "");
  }
  const uint32_t tMkdir = millis();

  const char* cacheRoot = CasperPaths::kPackageCacheRoot;
  auto epub = makeUniqueNoThrow<Epub>(path, cacheRoot);
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  SystemLog::logTiming("OPEN", "pre exists=%lu mkdir=%lu", static_cast<unsigned long>(tExists - tStep0),
                       static_cast<unsigned long>(tMkdir - tExists));
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds.
  // Upper-left status (not center pill). Cached open → no cue.
  const uint32_t tBeforeProbe = millis();
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // Delete Cache used to drop book.bin and leave rivulet/*.rvpm (open-handle
    // rmdir). Resume then reloaded a finished chapter map and never rebuilt.
    const std::string rivulet = CasperBook::rivuletDirForPath(path);
    if (Storage.exists(rivulet.c_str())) {
      const bool wiped = wipeCacheDirectory(rivulet);
      LOG_INF("READER", "book.bin miss — wipe leftover rivulet=%d %s", wiped ? 1 : 0, rivulet.c_str());
      SystemLog::logTiming("CACHE", "miss wipe rivulet=%d", wiped ? 1 : 0);
    }
  }
  if (uncached && !hasOpenHints()) {
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/false);
  }
  const uint32_t tProbe = millis();
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). Rivulet always needs maxAlloc for first-chapter ZIP inflate.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    loan.emplace(renderer);
    const uint32_t t0 = millis();
    // Rivulet does not consume publisher CSS (HTML→IR tags only) — skip CSS load
    // to save heap for inflate + convert. Styling comes from HTML tags + Settings.
    loaded = epub->load(true, /*skipLoadingCss=*/true);
    LOG_DBG("READER", "epub->load %s in %lums (book.bin %s)", path.c_str(), static_cast<unsigned long>(millis() - t0),
            uncached ? "miss" : "hit");
    if (QrTimingLog::active()) {
      QrTimingLog::line("epub->load %lums book.bin=%s", static_cast<unsigned long>(millis() - t0),
                        uncached ? "MISS" : "HIT");
    }
    SystemLog::logTimed("EPUB", millis() - t0, "load book.bin=%s", uncached ? "MISS" : "HIT");
    SystemLog::logTiming("OPEN", "probe=%lu loan+load=%lu total=%lu", static_cast<unsigned long>(tProbe - tBeforeProbe),
                         static_cast<unsigned long>(millis() - tProbe), static_cast<unsigned long>(millis() - tStep0));
  }
  if (loaded) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, CasperPaths::kPackageCacheRoot);
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, CasperPaths::kPackageCacheRoot);
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // From a book: that folder. Otherwise SD root (books live on card root).
  auto initialPath = fromBookPath.empty() ? std::string{"/"} : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  LOG_INF("READER", "Opening with Rivulet engine: %s", epubPath.c_str());
  activityManager.swapActivity(std::make_unique<RivuletReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.swapActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.swapActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.swapActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  const uint32_t tEnter = millis();
  if (QrTimingLog::active()) QrTimingLog::line("ReaderActivity::onEnter start");
  SystemLog::logTiming("READER", "open start");
  // Home Read already FASTs/HALFs "Opening". Cold entry (no open hints): show
  // Loading on glass so the wait is never silent.
  if (!hasOpenHints()) {
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
  }
  sdFontSystem.ensureLoaded(renderer);
  LOG_DBG("READER", "ensureLoaded %lums", static_cast<unsigned long>(millis() - tEnter));
  if (QrTimingLog::active()) {
    QrTimingLog::line("after ensureLoaded fonts (+%lums step)", static_cast<unsigned long>(millis() - tEnter));
  }
  SystemLog::logTimed("READER", millis() - tEnter, "ensureLoaded fonts");

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    const uint32_t tLoad = millis();
    auto epub = loadEpub(initialBookPath);
    if (QrTimingLog::active()) {
      QrTimingLog::line("after loadEpub (+%lums step)", static_cast<unsigned long>(millis() - tLoad));
    }
    if (!epub) {
      onGoBack();
      return;
    }
    LOG_DBG("READER", "ReaderActivity open total %lums before EpubReader",
            static_cast<unsigned long>(millis() - tEnter));
    if (QrTimingLog::active()) {
      QrTimingLog::line("before EpubReaderActivity (ReaderActivity total +%lums)",
                        static_cast<unsigned long>(millis() - tEnter));
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
