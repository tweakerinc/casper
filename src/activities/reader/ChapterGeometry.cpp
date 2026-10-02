#include "ChapterGeometry.h"
#include <Epub.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <Esp.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <RivuletEngine.h>
#include <algorithm>
#include <string>
namespace chaptergeometry {
size_t prepareRange(Epub& epub, GfxRenderer& renderer, rivulet::RivuletEngine& target,
             const std::string& spineHref, uint8_t imageMode,size_t first,size_t count) {


  auto& chapter = target.chapterMutable();
  auto& blocks = chapter.blocksMutable();
  if (blocks.empty()) return 0;
  const size_t last=std::min(blocks.size(),first+std::min(count,blocks.size()));
  // Suppress / Placeholder bake at convert; still zero plates if stale IR slipped through.
  if (imageMode == 2 ||
      imageMode == 1) {
    int n = 0;
    for (size_t bi=first; bi<last; ++bi) {
      if (chapter.blocks()[bi].kind != rivulet::BlockKind::Image) continue;
      auto& b=blocks[bi];
      b.imageW = 0;
      b.imageH = 0;
      ++n;
    }
    if (n > 0) {
      LOG_INF("RVR", "prepareChapterImages skip %d plates (imageRendering=%u)", n,
              static_cast<unsigned>(imageMode));
    }
    return last;
  }

  // Directory of the HTML spine item (OEBPS/Text/ch.xhtml → OEBPS/Text/).
  std::string baseDir;
  {
    const auto slash = spineHref.find_last_of('/');
    if (slash != std::string::npos) baseDir = spineHref.substr(0, slash + 1);
  }
  const int viewW = std::max(32, static_cast<int>(target.renderKey().viewportW));
  const int viewH = std::max(32, static_cast<int>(target.renderKey().viewportH));
  const auto& runs = chapter.runs();
  int prepared = 0;
  int skipped = 0;
  // Resolve HTML-relative or package-absolute href → ZIP path inside the EPUB.
  // Tries candidates against the ZIP (getItemSize) so we never prefix baseDir onto
  // an already-absolute path (OEBPS/Text/ + OEBPS/Images/… → broken double path).
  auto resolveItemPath = [&](const std::string& rel) -> std::string {
    if (rel.empty()) return {};
    const std::string decoded = FsHelpers::decodeUriEscapes(rel);
    std::string cands[4];
    int nc = 0;
    auto add = [&](std::string p) {
      p = FsHelpers::normalisePath(std::move(p));
      if (p.empty()) return;
      for (int i = 0; i < nc; ++i) {
        if (cands[i] == p) return;
      }
      if (nc < 4) cands[nc++] = std::move(p);
    };
    // Relative to chapter HTML (../Images/orn.png).
    if (!decoded.empty() && decoded[0] == '.') {
      add(baseDir + decoded);
    }
    // Package-absolute (rewritten IR) or bare path from HTML.
    add(decoded);
    if (!baseDir.empty()) add(baseDir + decoded);
    // Prefer a path that actually exists in the EPUB zip.
    size_t itemSz = 0;
    for (int i = 0; i < nc; ++i) {
      if (epub.getItemSize(cands[i], &itemSz) && itemSz > 0) return cands[i];
    }
    return nc > 0 ? cands[0] : std::string{};
  };

  for (size_t bi = first; bi < last; ++bi) {
    const rivulet::Block source = chapter.blocks()[bi];
    if (source.kind != rivulet::BlockKind::Image || source.runCount == 0) continue;
    auto& b = blocks[bi];
    if (b.runBegin >= runs.size()) continue;
    std::string rel = chapter.runString(runs[b.runBegin]);
    if (rel.empty()) continue;
    std::string resolved = resolveItemPath(rel);
    if (resolved.empty()) {
      LOG_ERR("RVR", "image resolve fail rel=%s base=%s", rel.c_str(), baseDir.c_str());
      b.imageW = 0;
      b.imageH = 0;
      ++skipped;
      continue;
    }
    if (!ImageDecoderFactory::isFormatSupported(resolved)) {
      // SVG ornamental breaks etc. — leave 0×0 so layouter skips the plate and
      // the EPUB text fallback ("* * *") is the only ink (no hollow white box).
      LOG_DBG("RVR", "image skip unsupported %s", resolved.c_str());
      b.imageW = 0;
      b.imageH = 0;
      ++skipped;
      continue;
    }

    ImageDimensions dims{0, 0};
    ImageDimsProbe probe;
    const bool streamOk = epub.readItemContentsToStream(resolved, probe, 1024, /*allowEarlyStop=*/true);
    if (!probe.getDimensions(dims) || dims.width <= 0 || dims.height <= 0) {
      // Do not invent a viewport-sized box. Hail Mary chapter plates that fail
      // the probe used to layout as a hollow rectangle with no title ink.
      LOG_ERR("RVR", "image dims probe fail streamOk=%d path=%s — skip plate", streamOk ? 1 : 0, resolved.c_str());
      b.imageW = 0;
      b.imageH = 0;
      ++skipped;
      continue;
    }

    int iw = dims.width;
    int ih = dims.height;
    // CSS width from figleft/figright (stored on block before probe) wins as display width.
    int cssW = b.imageW > 0 ? static_cast<int>(b.imageW) : 0;
    bool leftFloat = (b.flags & rivulet::kBlockFloatLeft) != 0;
    bool rightFloat = (b.flags & rivulet::kBlockFloatRight) != 0;
    const bool isOrnament = (b.flags & rivulet::kBlockOrnament) != 0;
    // Illuminae briefings are ~723px document plates with CSS float:right; width:40%.
    // On e-ink 40% is unreadable. Large document floats become nearly full-width
    // centered plates; only small stamps stay as true side floats.
    const bool docFloatPlate = (leftFloat || rightFloat) && !isOrnament && dims.width >= 280 && dims.height >= 120;
    if (docFloatPlate) {
      b.flags = static_cast<uint16_t>(b.flags & ~(rivulet::kBlockFloatLeft | rivulet::kBlockFloatRight));
      leftFloat = false;
      rightFloat = false;
      cssW = std::max(240, (viewW * 94) / 100);
    } else if ((leftFloat || rightFloat) && dims.width > 0) {
      // Small float icons / email stamps — keep a modest side column.
      const int floatCap = std::max(80, (viewW * 45) / 100);
      if (cssW <= 0 || cssW > floatCap || cssW < 40) {
        cssW = floatCap;
      }
    }
    if (cssW > 0 && cssW <= viewW && dims.width > 0) {
      // Scale natural aspect to CSS width (Alice figleft 80 / figright 183).
      ih = std::max(1, (dims.height * cssW) / std::max(1, static_cast<int>(dims.width)));
      iw = cssW;
    }
    // Fit width; cap height so a single plate never exceeds ~90% of the page.
    if (iw > viewW) {
      ih = std::max(1, (ih * viewW) / iw);
      iw = viewW;
    }
    const int maxH = (viewH * 9) / 10;
    if (ih > maxH) {
      iw = std::max(1, (iw * maxH) / ih);
      ih = maxH;
    }
    // Chapter ornaments (.orn img { width: 12% }) — never full-page plates.
    if (isOrnament && dims.width > 0) {
      const int targetW = std::max(28, (viewW * 12) / 100);
      ih = std::max(1, (dims.height * targetW) / std::max(1, static_cast<int>(dims.width)));
      iw = targetW;
    }

    // Letter-shrink ONLY narrow LEFT floats (Alice ornate C / figleft). Never
    // invent a float for a small centered ornament — v0.1.9 wrapped chapter
    // flourishes into the first body lines and left-aligned the title.
    const int bodyLineEst = std::max(18, renderer.getLineHeight(target.renderKey().fontId, 1.0f));
    const int maxLetterW = std::max(120, (viewW * 28) / 100);
    const bool letterGlyph = !isOrnament && leftFloat && iw > 0 && iw <= maxLetterW;
    if (letterGlyph) {
      const int targetH = bodyLineEst * 2;
      if (ih > targetH && ih > 0) {
        iw = std::max(1, (iw * targetH) / ih);
        ih = targetH;
      }
    }
    b.imageW = static_cast<uint16_t>(std::min(65535, iw));
    b.imageH = static_cast<uint16_t>(std::min(65535, ih));
    ++prepared;
    LOG_INF("RVR", "image[%u] %s %dx%d (src %dx%d) orn=%d L=%d R=%d", static_cast<unsigned>(bi), resolved.c_str(), iw,
            ih, static_cast<int>(dims.width), static_cast<int>(dims.height), isOrnament ? 1 : 0, leftFloat ? 1 : 0,
            rightFloat ? 1 : 0);

    // Store package-absolute path on the IR run so paint/extract never depend on
    // baseDir + "../Images/..." re-resolution (fragile after IR cache reload).
    if (resolved != rel) {
      (void)chapter.setRunText(b.runBegin, resolved);
    }
  }
  if (prepared > 0 || skipped > 0) {
    LOG_INF("RVR", "prepareChapterImages spine=%s prepared=%d skipped=%d free=%u maxA=%u", spineHref.c_str(), prepared,
            skipped, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }

  return last;
}
void prepare(Epub& epub,GfxRenderer& renderer,rivulet::RivuletEngine& target,const std::string& href,uint8_t mode) {
  (void)prepareRange(epub,renderer,target,href,mode,0,target.chapter().blockCount());
}
} // namespace chaptergeometry
