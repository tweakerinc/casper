#include "ChapterLoader.h"

#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "util/TaskWatchdog.h"

namespace chapterload {
namespace {

// Chapter HTML held in RAM during convert. The convert needs headroom on top of
// this, so the cap is about the pair fitting, not about the file alone.
constexpr size_t kMaxHtml = 160 * 1024;

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
  // home-cover PNG / font cache keeps maxAlloc and the convert aborts midway.
  const bool hasCachedIr = Storage.exists(irPath);
  const bool needAggressive =
      requireCompleteIr || !hasCachedIr || ESP.getMaxAllocHeap() < 48 * 1024 || ESP.getFreeHeap() < 60 * 1024;
  if (!hasCachedIr || eng.hasChapter() || needAggressive) {
    prepHeap(needAggressive || requireCompleteIr);
  }

  bool ok = false;
  bool fromIrCache = false;

  // --- Cached IR ------------------------------------------------------------
  if (Storage.exists(irPath)) {
    ok = eng.loadIr(irPath);
    if (ok && Storage.exists(htmlPath)) {
      // Reject truncated IR from an older OOM convert: if the HTML on SD is much
      // larger than the IR text, the cache is a short chapter with a false end.
      HalFile hf;
      if (Storage.openFileForRead("CHLOAD", htmlPath, hf)) {
        const size_t htmlSz = hf.size();
        hf.close();
        const size_t textSz = eng.chapter().textSize();
        const bool shortVsHtml =
            (htmlSz > 8000 && textSz > 0 && textSz * 5 / 2 < htmlSz) || (htmlSz > 20000 && textSz < 5000);
        if (shortVsHtml) {
          LOG_ERR("CHLOAD", "stale short IR spine=%d text=%u html=%u — reconvert", spineIndex,
                  static_cast<unsigned>(textSz), static_cast<unsigned>(htmlSz));
          eng.clear();
          Storage.remove(irPath);
          if (Storage.exists(mapPath)) Storage.remove(mapPath);
          ok = false;
        }
      }
    }
    if (ok) {
      fromIrCache = true;
      LOG_DBG("CHLOAD", "loaded IR %s", irPath);
      if (Storage.exists(mapPath) && eng.loadPageMap(mapPath)) {
        if (eng.scrubStaleCompleteMap(rend)) {
          LOG_DBG("CHLOAD", "page map %s scrubbed pages=%d complete=%d", mapPath, eng.mapKnownPages(),
                  eng.mapComplete() ? 1 : 0);
          // Only discard the file when the scrub emptied it (false 2–3 page
          // complete). An incomplete reopen must keep it so idle can finish.
          if (eng.mapKnownPages() <= 1 && Storage.exists(mapPath)) Storage.remove(mapPath);
        }
      }
    } else if (Storage.exists(irPath)) {
      LOG_DBG("CHLOAD", "stale/bad IR %s — reconvert", irPath);
      Storage.remove(irPath);
    }
  }

  // --- Convert from HTML ----------------------------------------------------
  if (!ok) {
    if (aborted()) {
      LOG_INF("CHLOAD", "spine %d abort before convert", spineIndex);
      return out;
    }
    // Never accumulate chapter HTML in a std::string: growth aborts on OOM under
    // -fno-exceptions. ZIP-inflate to SD, then load with makeUniqueNoThrow.
    // Hold the framebuffer loan across stream + load + convert so maxAlloc stays
    // high for the whole sequence.
    {
      GfxRenderer::FrameBufferLoan loan(rend, lendFb);
      LOG_INF("CHLOAD", "html phase loan=%d free=%u maxAlloc=%u", lendFb ? 1 : 0,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

      // Two passes: prefer the cached sN.html; on failure delete and re-stream
      // (a stale/truncated extract left chapters permanently unloadable).
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
            if (fileSize > kMaxHtml) {
              Storage.remove(tmpHtmlPath);
              LOG_ERR("CHLOAD", "spine %d HTML too large (%u) — skip", spineIndex, static_cast<unsigned>(fileSize));
              return out;
            }
            readPath = Storage.rename(tmpHtmlPath, htmlPath) ? htmlPath : tmpHtmlPath;
            LOG_DBG("CHLOAD", "spine %d htmlBytes=%u", spineIndex, static_cast<unsigned>(fileSize));
          }
          if (!streamed) {
            if (forceRestream) return out;
            continue;
          }
        }

        HalFile htmlFile;
        if (!Storage.openFileForRead("CHLOAD", readPath, htmlFile)) {
          LOG_ERR("CHLOAD", "spine %d open HTML failed", spineIndex);
          if (Storage.exists(htmlPath)) Storage.remove(htmlPath);
          continue;
        }
        const size_t htmlSize = htmlFile.size();
        if (htmlSize == 0 || htmlSize > kMaxHtml) {
          htmlFile.close();
          LOG_DBG("CHLOAD", "spine %d bad html size %u — discard", spineIndex, static_cast<unsigned>(htmlSize));
          if (Storage.exists(htmlPath)) Storage.remove(htmlPath);
          continue;
        }
        auto htmlBuf = makeUniqueNoThrow<uint8_t[]>(htmlSize + 1);
        if (!htmlBuf) {
          htmlFile.close();
          LOG_ERR("CHLOAD", "spine %d HTML OOM size=%u free=%u maxAlloc=%u", spineIndex,
                  static_cast<unsigned>(htmlSize), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
          return out;  // keep the HTML: a later attempt may have more heap
        }
        const int got = htmlFile.read(htmlBuf.get(), htmlSize);
        htmlFile.close();
        if (got < 0 || static_cast<size_t>(got) != htmlSize) {
          LOG_ERR("CHLOAD", "spine %d HTML short read %d/%u", spineIndex, got, static_cast<unsigned>(htmlSize));
          if (Storage.exists(htmlPath)) Storage.remove(htmlPath);
          continue;
        }
        htmlBuf[htmlSize] = 0;

        if (aborted()) {
          LOG_INF("CHLOAD", "spine %d abort before ingestHtml", spineIndex);
          return out;
        }
        resetTaskWatchdogIfSubscribed();

        // free/maxA here are net of the HTML buffer we just took.
        const size_t maxA = ESP.getMaxAllocHeap();
        const size_t freeH = ESP.getFreeHeap();
        if (maxA < 12 * 1024 || freeH < 14 * 1024) {
          LOG_ERR("CHLOAD", "spine %d convert skip low heap free=%u maxA=%u html=%u", spineIndex,
                  static_cast<unsigned>(freeH), static_cast<unsigned>(maxA), static_cast<unsigned>(htmlSize));
          htmlBuf.reset();
          eng.clear();
          delay(20);
          continue;
        }

        const uint32_t t0 = millis();
        ok = eng.ingestHtml(reinterpret_cast<const char*>(htmlBuf.get()), htmlSize, /*irPathSave=*/nullptr,
                            /*armDropCapFirstPara=*/false, imageRendering);
        // Partial OOM: clear, yield, retry once while still holding HTML + loan.
        if (ok && eng.chapter().failed() && pass == 0) {
          LOG_ERR("CHLOAD", "spine %d partial IR — retry convert free=%u maxA=%u", spineIndex,
                  static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
          eng.clear();
          delay(30);
          yield();
          if (ESP.getMaxAllocHeap() >= 12 * 1024) {
            ok = eng.ingestHtml(reinterpret_cast<const char*>(htmlBuf.get()), htmlSize, nullptr, false, imageRendering);
          }
        }
        htmlBuf.reset();  // free before the caller lays anything out
        LOG_INF("CHLOAD", "ingestHtml %s partial=%d in %lums blocks=%u text=%u html=%u free=%u maxA=%u",
                ok ? "ok" : "FAIL", (ok && eng.chapter().failed()) ? 1 : 0, static_cast<unsigned long>(millis() - t0),
                static_cast<unsigned>(eng.chapter().blockCount()), static_cast<unsigned>(eng.chapter().textSize()),
                static_cast<unsigned>(htmlSize), static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));

        if (ok && !eng.chapter().failed()) {
          prepImages(item.href);
          // Persist only a complete IR — a partial must not become permanent.
          if (irPath[0]) (void)eng.chapter().saveToFile(irPath);
          // Drop any map built from an earlier partial session for this spine.
          if (Storage.exists(mapPath)) Storage.remove(mapPath);
        } else if (ok && eng.chapter().failed()) {
          if (Storage.exists(irPath)) Storage.remove(irPath);
          if (Storage.exists(mapPath)) Storage.remove(mapPath);
          if (requireCompleteIr) {
            LOG_ERR("CHLOAD", "spine %d partial IR refused (requireFull) text=%u html=%u", spineIndex,
                    static_cast<unsigned>(eng.chapter().textSize()), static_cast<unsigned>(htmlSize));
            eng.clear();
            ok = false;
            break;  // leave the loan scope for an aggressive scrub + one retry
          }
          prepImages(item.href);
          LOG_ERR("CHLOAD", "spine %d using partial IR (not cached) text=%u html=%u", spineIndex,
                  static_cast<unsigned>(eng.chapter().textSize()), static_cast<unsigned>(htmlSize));
        } else if (Storage.exists(htmlPath)) {
          Storage.remove(htmlPath);  // bad extract — force ZIP re-stream next pass
        }
      }
    }

    // requireComplete: one more attempt after a hard scrub, outside the loan.
    if (!ok && requireCompleteIr && Storage.exists(htmlPath)) {
      prepHeap(/*aggressive=*/true);
      GfxRenderer::FrameBufferLoan loan(rend, lendFb);
      HalFile htmlFile;
      if (Storage.openFileForRead("CHLOAD", htmlPath, htmlFile)) {
        const size_t htmlSize = htmlFile.size();
        if (htmlSize > 0 && htmlSize <= kMaxHtml) {
          auto htmlBuf = makeUniqueNoThrow<uint8_t[]>(htmlSize + 1);
          if (htmlBuf) {
            const int got = htmlFile.read(htmlBuf.get(), htmlSize);
            htmlFile.close();
            if (got >= 0 && static_cast<size_t>(got) == htmlSize) {
              htmlBuf[htmlSize] = 0;
              LOG_INF("CHLOAD", "spine %d requireFull reconvert free=%u maxA=%u", spineIndex,
                      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
              ok = eng.ingestHtml(reinterpret_cast<const char*>(htmlBuf.get()), htmlSize, nullptr, false,
                                  imageRendering);
              htmlBuf.reset();
              if (ok && !eng.chapter().failed()) {
                prepImages(item.href);
                if (irPath[0]) (void)eng.chapter().saveToFile(irPath);
                if (Storage.exists(mapPath)) Storage.remove(mapPath);
                LOG_INF("CHLOAD", "spine %d requireFull OK text=%u", spineIndex,
                        static_cast<unsigned>(eng.chapter().textSize()));
              } else {
                LOG_ERR("CHLOAD", "spine %d requireFull still partial/fail", spineIndex);
                eng.clear();
                ok = false;
                if (Storage.exists(irPath)) Storage.remove(irPath);
              }
            } else {
              htmlFile.close();
            }
          } else {
            htmlFile.close();
          }
        } else {
          htmlFile.close();
        }
      }
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
