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
#include "util/CasperBookStore.h"
#include "util/CasperPaths.h"
#include "util/QrTimingLog.h"
#include "util/SystemLog.h"

#include <BookPathId.h>
#include <functional>

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

namespace {

// Best-effort copy of a single file if dest missing.
bool copyFileIfMissing(const std::string& src, const std::string& dst) {
  if (!Storage.exists(src.c_str()) || Storage.exists(dst.c_str())) return false;
  // Ensure parent of dst
  const size_t slash = dst.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    Storage.ensureDirectoryExists(dst.substr(0, slash).c_str());
  }
  HalFile in, out;
  if (!Storage.openFileForRead("EPUB", src, in)) return false;
  if (!Storage.openFileForWrite("EPUB", dst, out)) {
    in.close();
    return false;
  }
  uint8_t buf[512];
  for (;;) {
    const int n = in.read(buf, sizeof(buf));
    if (n < 0) {
      in.close();
      out.close();
      Storage.remove(dst.c_str());
      return false;
    }
    if (n == 0) break;
    if (out.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      in.close();
      out.close();
      Storage.remove(dst.c_str());
      return false;
    }
  }
  out.flush();
  out.close();
  in.close();
  return true;
}

// Import classic epub_<std::hash> package into book_<pathId>/package once.
void importLegacyPackageIfNeeded(const std::string& path) {
  const std::string pkg = BookPathId::packageDir(path);
  if (Storage.exists((pkg + "/book.bin").c_str())) return;

  // Only rekey within /.casper (epub_<hash> → book_<id>/package). No /.casper.
  const std::string legacyCasper = BookPathId::legacyEpubHashDir(path, CasperPaths::kPackageCacheRoot);
  if (!Storage.exists((legacyCasper + "/book.bin").c_str())) return;

  Storage.ensureDirectoryExists(pkg.c_str());
  if (copyFileIfMissing(legacyCasper + "/book.bin", pkg + "/book.bin")) {
    LOG_INF("READER", "imported package book.bin → %s", pkg.c_str());
  }
  for (const char* name : {"cover.bmp", "thumb.bmp", "progress.bin", "progress.bin.bak"}) {
    (void)copyFileIfMissing(legacyCasper + "/" + name, pkg + "/" + name);
  }
  const std::string bookRoot = BookPathId::bookRoot(path);
  Storage.ensureDirectoryExists(bookRoot.c_str());
  (void)copyFileIfMissing(legacyCasper + "/progress.bin", bookRoot + "/progress.bin");
  (void)copyFileIfMissing(legacyCasper + "/stats_v6.bin", bookRoot + "/stats_v6.bin");
}

}  // namespace

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  // Unified /.crosspoint/book_<pathId>/package. Import classic epub_* once if needed.
  importLegacyPackageIfNeeded(path);
  (void)CasperBook::openBook(path, "", "");  // ensure epub_<hash> + rivulet dirs

  const char* cacheRoot = CasperPaths::kPackageCacheRoot;
  auto epub = makeUniqueNoThrow<Epub>(path, cacheRoot);
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds.
  // Upper-left status (not center pill). Cached open → no cue.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached && !hasOpenHints()) {
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/false);
  }
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
