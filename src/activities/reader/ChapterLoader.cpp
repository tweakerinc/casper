#include "ChapterLoader.h"

#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "util/CachedIrPolicy.h"
#include "util/ChapterLoadPolicy.h"
#include "util/TaskWatchdog.h"

namespace chapterload {
namespace {

constexpr size_t kMaxHtml = chapterloadpolicy::kMaxHtmlInRam;

}  // namespace

Result loadChapterIr(const Request& req, const Hooks& hooks) {
  Result out;
  if (!req.epub || !req.engine || !req.renderer || !hooks.prepareHeap) return out;

  Epub& epub = *req.epub;
  rivulet::RivuletEngine& eng = *req.engine;
  GfxRenderer& rend = *req.renderer;
  const int spineIndex = req.spineIndex;
  const std::string& irDir = req.irDir;
  const uint8_t imageRendering = req.imageRendering;
  const bool requireCompleteIr = req.requireCompleteIr;
  const bool lendFb = req.lendFrameBuffer;

  const auto prepHeap = [&](const bool aggressive) { hooks.prepareHeap(hooks.ctx, aggressive); };
  const auto prepImages = [&](const std::string& href) {
    if (hooks.prepareImages) hooks.prepareImages(hooks.ctx, href.c_str());
  };
  const auto aborted = [&]() {
    if (!hooks.shouldAbort) return false;
    resetTaskWatchdogIfSubscribed();
    return hooks.shouldAbort(hooks.ctx);
  };

  const int n = epub.getSpineItemsCount();
  if (spineIndex < 0 || spineIndex >= n) return out;

  const auto item = epub.getSpineItem(spineIndex);
  if (item.href.empty()) {
    LOG_DBG("CHLOAD", "spine %d empty href — skip", spineIndex);
    return out;
  }

  LOG_INF("CHLOAD", "load %d href=%s free=%u maxAlloc=%u requireFull=%d", spineIndex, item.href.c_str(),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          requireCompleteIr ? 1 : 0);

  if (!irDir.empty()) {
    Storage.ensureDirectoryExists(irDir.c_str());
    if (req.bindPageCache) {
      // Classic section.bin idea: persist laid-out pages under pages/ so resume
      // and revisit are deserialize + paint. MUST be spine-namespaced.
      const std::string pagesDir = irDir + "/pages";
      eng.setPageCacheDir(pagesDir.c_str());
      eng.setPageCacheSpine(spineIndex);
    } else {
      eng.clearPageCacheDir();
      eng.setPageCacheSpine(-1);
    }
  } else {
    eng.clearPageCacheDir();
    eng.setPageCacheSpine(-1);
  }

  char irPath[192];
  char htmlPath[192];
  char tmpHtmlPath[200];
  char mapPath[200];
  // IR/map fingerprint includes Images mode so changing Settings → Images does
  // not reuse IR that still has/omits plates.
  const unsigned imgMode = static_cast<unsigned>(imageRendering);
  std::snprintf(irPath, sizeof(irPath), "%s/s%d_m%u.rvir", irDir.c_str(), spineIndex, imgMode);
  std::snprintf(htmlPath, sizeof(htmlPath), "%s/s%d.html", irDir.c_str(), spineIndex);
  std::snprintf(tmpHtmlPath, sizeof(tmpHtmlPath), "%s/s%d.html.tmp", irDir.c_str(), spineIndex);
  std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir.c_str(), spineIndex, imgMode);

  // Always free retained chapter + image/font caches before a spine load, or a
  // home-cover PNG / font cache keeps maxAlloc and the convert (or IR malloc)
  // aborts. Cached deserialize is one contiguous text-blob alloc — 48KB looked
  // "fine" after a menu on X3 and still failed, then the old code deleted .rvir.
  const bool hasCachedIr = Storage.exists(irPath);
  const bool needAggressive =
      requireCompleteIr || !hasCachedIr || ESP.getMaxAllocHeap() < 64 * 1024 || ESP.getFreeHeap() < 72 * 1024;
  if (!hasCachedIr || eng.hasChapter() || needAggressive) {
    prepHeap(needAggressive || requireCompleteIr);
  }

  bool ok = false;
  bool fromIrCache = false;

  // Loan covers cache deserialize AND convert. Nested inner loans are inert.
  // Sitting open leaves this on so a 50 KB CrossPoint .rvir can load; idle
  // indexing leaves it off because the panel still holds the current page.
  GfxRenderer::FrameBufferLoan fbLoan(rend, lendFb);
  LOG_INF("CHLOAD", "heap loan=%d free=%u maxAlloc=%u", lendFb ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  auto tryLoadIrFile = [&](const char* path, const char* thisMapPath) -> bool {
    if (!path || !path[0] || !Storage.exists(path)) return false;
    bool loaded = eng.loadIr(path);
    if (!loaded && eng.lastIrLoadResult() == rivulet::RivuletEngine::IrLoadResult::Oom) {
      LOG_ERR("CHLOAD", "IR OOM spine=%d — scrub and retry, keeping cache free=%u maxA=%u", spineIndex,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      prepHeap(true);
      loaded = eng.loadIr(path);
    }
    if (loaded) {
      fromIrCache = true;
      LOG_DBG("CHLOAD", "loaded IR %s", path);
      if (thisMapPath && thisMapPath[0] && Storage.exists(thisMapPath) && eng.loadPageMap(thisMapPath)) {
        if (eng.scrubStaleCompleteMap(rend)) {
          LOG_DBG("CHLOAD", "page map %s scrubbed pages=%d complete=%d", thisMapPath, eng.mapKnownPages(),
                  eng.mapComplete() ? 1 : 0);
          // Only discard the file when the scrub emptied it (false 2–3 page
          // complete). An incomplete reopen must keep it so idle can finish.
          if (eng.mapKnownPages() <= 1 && Storage.exists(thisMapPath)) Storage.remove(thisMapPath);
        }
      }
      return true;
    }
    if (!Storage.exists(path)) return false;
    const auto loadSt = eng.lastIrLoadResult();
    const cachedir::LoadMiss miss = (loadSt == rivulet::RivuletEngine::IrLoadResult::Oom) ? cachedir::LoadMiss::Oom
                                    : (loadSt == rivulet::RivuletEngine::IrLoadResult::StaleVersion)
                                        ? cachedir::LoadMiss::StaleVersion
                                        : cachedir::LoadMiss::Corrupt;
    if (cachedir::deleteFileOnLoadMiss(miss)) {
      LOG_DBG("CHLOAD", "corrupt IR %s — reconvert", path);
      Storage.remove(path);
    } else if (loadSt == rivulet::RivuletEngine::IrLoadResult::Oom) {
      // Keep the file. Convert a prefix of HTML under the same loan — skipping
      // convert here is what left sitting opens as "Chapter not readable".
      LOG_ERR("CHLOAD", "IR OOM spine=%d — keep cache, convert", spineIndex);
    } else {
      LOG_ERR("CHLOAD", "IR stale version spine=%d — keep cache, try convert", spineIndex);
    }
    return false;
  };

  ok = tryLoadIrFile(irPath, mapPath);
  if (!ok && chapterloadpolicy::trySiblingImageModeCaches()) {
    for (uint8_t tryIndex = 1; tryIndex < chapterloadpolicy::kImageModeCount && !ok; ++tryIndex) {
      const unsigned m = chapterloadpolicy::cacheModeToTry(static_cast<uint8_t>(imgMode), tryIndex);
      char sibIr[192];
      char sibMap[200];
      std::snprintf(sibIr, sizeof(sibIr), "%s/s%d_m%u.rvir", irDir.c_str(), spineIndex, m);
      std::snprintf(sibMap, sizeof(sibMap), "%s/s%d_m%u.rvpm", irDir.c_str(), spineIndex, m);
      if (tryLoadIrFile(sibIr, sibMap)) {
        LOG_INF("CHLOAD", "spine %d using sibling IR mode %u (want %u)", spineIndex, m, imgMode);
        ok = true;
      }
    }
  }

  auto leftoverHeap = [&]() -> size_t {
    size_t leftover = ESP.getMaxAllocHeap();
    const size_t freeH = ESP.getFreeHeap();
    if (freeH < leftover) leftover = freeH;
    return leftover;
  };

  auto convertStoredHtml = [&](const char* path, const bool retryPartial) -> bool {
    HalFile htmlFile;
    if (!Storage.openFileForRead("CHLOAD", path, htmlFile)) {
      LOG_ERR("CHLOAD", "spine %d open HTML failed", spineIndex);
      return false;
    }
    const size_t fileSize = htmlFile.size();
    if (fileSize == 0) {
      htmlFile.close();
      LOG_DBG("CHLOAD", "spine %d bad html size 0 — discard", spineIndex);
      return false;
    }
    size_t htmlCap = chapterloadpolicy::htmlBytesToConvert(fileSize, kMaxHtml, leftoverHeap(),
                                                          chapterloadpolicy::kConvertHeadroom);
    if (htmlCap == 0) {
      htmlFile.close();
      LOG_ERR("CHLOAD", "spine %d HTML cap 0 leftover=%u file=%u", spineIndex,
              static_cast<unsigned>(leftoverHeap()), static_cast<unsigned>(fileSize));
      return false;
    }
    auto htmlBuf = makeUniqueNoThrow<uint8_t[]>(htmlCap + 1);
    if (!htmlBuf && htmlCap > 1024) {
      htmlCap /= 2;
      htmlBuf = makeUniqueNoThrow<uint8_t[]>(htmlCap + 1);
    }
    if (!htmlBuf) {
      htmlFile.close();
      LOG_ERR("CHLOAD", "spine %d HTML OOM size=%u free=%u maxAlloc=%u", spineIndex, static_cast<unsigned>(htmlCap),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      return false;
    }
    const int got = htmlFile.read(htmlBuf.get(), htmlCap);
    htmlFile.close();
    if (got <= 0) {
      LOG_ERR("CHLOAD", "spine %d HTML short read %d/%u", spineIndex, got, static_cast<unsigned>(htmlCap));
      return false;
    }
    const size_t htmlSize = static_cast<size_t>(got);
    htmlBuf[htmlSize] = 0;
    if (htmlSize < fileSize) {
      LOG_INF("CHLOAD", "spine %d converting prefix %u of %u", spineIndex, static_cast<unsigned>(htmlSize),
              static_cast<unsigned>(fileSize));
    }

    if (aborted()) {
      LOG_INF("CHLOAD", "spine %d abort before ingestHtml", spineIndex);
      return false;
    }
    resetTaskWatchdogIfSubscribed();

    const uint32_t t0 = millis();
    bool conv = eng.ingestHtml(reinterpret_cast<const char*>(htmlBuf.get()), htmlSize, /*irPathSave=*/nullptr,
                               /*armDropCapFirstPara=*/false, imageRendering);
    if (conv && eng.chapter().failed() && retryPartial) {
      LOG_ERR("CHLOAD", "spine %d partial IR — retry convert free=%u maxA=%u", spineIndex,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      eng.clear();
      delay(30);
      yield();
      conv = eng.ingestHtml(reinterpret_cast<const char*>(htmlBuf.get()), htmlSize, nullptr, false, imageRendering);
    }
    htmlBuf.reset();
    LOG_INF("CHLOAD", "ingestHtml %s partial=%d in %lums blocks=%u text=%u html=%u free=%u maxA=%u",
            conv ? "ok" : "FAIL", (conv && eng.chapter().failed()) ? 1 : 0, static_cast<unsigned long>(millis() - t0),
            static_cast<unsigned>(eng.chapter().blockCount()), static_cast<unsigned>(eng.chapter().textSize()),
            static_cast<unsigned>(htmlSize), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return conv;
  };

  auto commitConvert = [&]() {
    if (ok && !eng.chapter().failed()) {
      prepImages(item.href);
      if (irPath[0]) (void)eng.chapter().saveToFile(irPath);
      if (Storage.exists(mapPath)) Storage.remove(mapPath);
    } else if (ok && eng.chapter().failed()) {
      if (requireCompleteIr) {
        LOG_ERR("CHLOAD", "spine %d partial IR refused (requireFull) text=%u", spineIndex,
                static_cast<unsigned>(eng.chapter().textSize()));
        eng.clear();
        ok = false;
      } else {
        prepImages(item.href);
        LOG_ERR("CHLOAD", "spine %d using partial IR (not cached) text=%u", spineIndex,
                static_cast<unsigned>(eng.chapter().textSize()));
      }
    }
  };

  if (!ok) {
    if (aborted()) {
      LOG_INF("CHLOAD", "spine %d abort before convert", spineIndex);
      return out;
    }

    for (int pass = 0; pass < 2 && !ok; ++pass) {
      const bool forceRestream = (pass > 0);
      if (forceRestream) {
        if (Storage.exists(htmlPath)) Storage.remove(htmlPath);
        if (Storage.exists(tmpHtmlPath)) Storage.remove(tmpHtmlPath);
        LOG_INF("CHLOAD", "spine %d re-stream HTML after convert/read fail", spineIndex);
      }

      const char* readPath = htmlPath;
      if (forceRestream || !Storage.exists(htmlPath)) {
        bool streamed = false;
        for (int attempt = 0; attempt < 3 && !streamed; ++attempt) {
          if (attempt > 0) delay(50);
          if (Storage.exists(tmpHtmlPath)) Storage.remove(tmpHtmlPath);
          HalFile tmpHtml;
          if (!Storage.openFileForWrite("CHLOAD", tmpHtmlPath, tmpHtml)) {
            LOG_ERR("CHLOAD", "spine %d open tmp HTML failed", spineIndex);
            continue;
          }
          streamed = epub.readItemContentsToStream(item.href, tmpHtml, 8192);
          const uint32_t fileSize = tmpHtml.size();
          tmpHtml.close();
          if (!streamed) {
            if (Storage.exists(tmpHtmlPath)) Storage.remove(tmpHtmlPath);
            LOG_ERR("CHLOAD", "spine %d ZIP stream fail free=%u maxAlloc=%u", spineIndex,
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
            continue;
          }
          if (fileSize == 0) {
            Storage.remove(tmpHtmlPath);
            LOG_DBG("CHLOAD", "spine %d HTML empty — skip", spineIndex);
            return out;
          }
          if (fileSize > kMaxHtml && !chapterloadpolicy::skipOversizedHtml()) {
            LOG_INF("CHLOAD", "spine %d HTML %u — convert prefix", spineIndex, static_cast<unsigned>(fileSize));
          }
          readPath = Storage.rename(tmpHtmlPath, htmlPath) ? htmlPath : tmpHtmlPath;
          LOG_DBG("CHLOAD", "spine %d htmlBytes=%u", spineIndex, static_cast<unsigned>(fileSize));
        }
        if (!streamed) {
          if (forceRestream) break;
          continue;
        }
      }

      ok = convertStoredHtml(readPath, /*retryPartial=*/pass == 0);
      if (aborted()) {
        LOG_INF("CHLOAD", "spine %d abort during convert", spineIndex);
        return out;
      }
      if (ok) {
        commitConvert();
        if (!ok && requireCompleteIr) break;
      } else if (Storage.exists(htmlPath) && pass == 0) {
        Storage.remove(htmlPath);  // bad extract — force ZIP re-stream next pass
      }
    }

    if (!ok && chapterloadpolicy::extraConvertRetry(lendFb, requireCompleteIr) && Storage.exists(htmlPath)) {
      prepHeap(/*aggressive=*/true);
      LOG_INF("CHLOAD", "spine %d extra convert free=%u maxA=%u", spineIndex,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      ok = convertStoredHtml(htmlPath, /*retryPartial=*/true);
      if (ok) commitConvert();
    }
  } else if (fromIrCache) {
    // Always re-size images for the current viewport: cached IR may hold float
    // widths from an older policy. Header-only probe, so do not rewrite the IR.
    prepImages(item.href);
  }

  if (!ok) {
    LOG_ERR("CHLOAD", "spine %d IR load failed", spineIndex);
    return out;
  }

  out.ok = true;
  out.fromCache = fromIrCache;
  out.partial = eng.chapter().failed();
  return out;
}

}  // namespace chapterload
