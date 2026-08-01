#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <optional>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
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
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    const uint32_t t0 = millis();
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
    LOG_DBG("READER", "epub->load %s in %lums (book.bin %s)", path.c_str(),
            static_cast<unsigned long>(millis() - t0), uncached ? "miss" : "hit");
    if (QrTimingLog::active()) {
      QrTimingLog::line("epub->load %lums book.bin=%s", static_cast<unsigned long>(millis() - t0),
                        uncached ? "MISS" : "HIT");
    }
    SystemLog::logTimed("EPUB", millis() - t0, "load book.bin=%s", uncached ? "MISS" : "HIT");
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

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
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

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
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
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  // Swap keeps a stacked Home alive (Phase 2 goHome fast path).
  activityManager.swapActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
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
  sdFontSystem.ensureLoaded(renderer);
  LOG_DBG("READER", "ensureLoaded %lums", static_cast<unsigned long>(millis() - tEnter));
  if (QrTimingLog::active()) {
    QrTimingLog::line("after ensureLoaded fonts (+%lums step)",
                      static_cast<unsigned long>(millis() - tEnter));
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
