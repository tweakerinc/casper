#include "LaidOutPage.h"

#include <Esp.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace rivulet {
namespace {

constexpr char kPageMagic[4] = {'R', 'V', 'P', 'G'};
// v2: page carries drawn thematic-break rules (RulePlate) as well as spans/images.
// v3: no leading-space indent / last-line justify
// v4: chapter titles stay centered; small ornaments are not letter-floated
constexpr uint16_t kPageFormatVersion = 4;
// Soft caps — a pathological page should not allocate unbounded on load.
constexpr uint32_t kMaxSpans = 2000;
constexpr uint32_t kMaxImages = 64;
constexpr uint32_t kMaxRules = 64;
// One GlyphSpan is a style run, not the whole page. 256 UTF-8 bytes is plenty;
// tryReadString used to allow 16KB then std::string::resize aborted on OOM.
constexpr uint32_t kMaxTextBytes = 256;
constexpr uint32_t kMaxHrefBytes = 256;
// x,y,fontId,style,scale + uint32 length (empty text).
constexpr uint32_t kMinOnDiskSpanBytes = 14;

// Device crash (v0.1.9, 2026-08-23): idle warmBehindPage loaded
// s11_p9_*.rvpg after a Hangul page filled the font cache. vector::reserve /
// string::resize used throwing new → abort() (PC in the loadFromFile frame).
// Probe a real block, same as ChapterIr, so we fail soft instead of rebooting.
bool canAlloc(const size_t bytes) {
  if (bytes == 0) return true;
  if (ESP.getMaxAllocHeap() < bytes + 512) return false;
  void* p = std::malloc(bytes);
  if (!p) return false;
  std::free(p);
  return true;
}

bool writeCursor(HalFile& f, const IrCursor& c) {
  return serialization::tryWritePod(f, c.blockIndex) && serialization::tryWritePod(f, c.runIndex) &&
         serialization::tryWritePod(f, c.byteInRun);
}

bool readCursor(HalFile& f, IrCursor& c) {
  return serialization::tryReadPod(f, c.blockIndex) && serialization::tryReadPod(f, c.runIndex) &&
         serialization::tryReadPod(f, c.byteInRun);
}

}  // namespace

bool LaidOutPage::saveToFile(const char* path, const RenderKey& key, const int pageIndex) const {
  if (!path || !*path || pageIndex < 0) return false;
  // Atomic: .tmp + rename so a power loss cannot leave a half-written page that
  // later deserializes into a partly-blank page.
  char tmpPath[240];
  const int wrote = std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  const bool useTmp = wrote > 0 && static_cast<size_t>(wrote) < sizeof(tmpPath);
  const char* writePath = useTmp ? tmpPath : path;
  if (useTmp && Storage.exists(tmpPath)) Storage.remove(tmpPath);
  HalFile f;
  if (!Storage.openFileForWrite("RVPG", writePath, f)) return false;

  bool ok = serialization::tryWritePod(f, kPageMagic);
  ok = ok && serialization::tryWritePod(f, kPageFormatVersion);
  ok = ok && serialization::tryWritePod(f, key);
  ok = ok && serialization::tryWritePod(f, static_cast<int32_t>(pageIndex));
  ok = ok && writeCursor(f, start);
  ok = ok && writeCursor(f, end);
  ok = ok && serialization::tryWritePod(f, contentH);
  ok = ok && serialization::tryWritePod(f, static_cast<uint8_t>(atChapterEnd ? 1 : 0));
  ok = ok && serialization::tryWritePod(f, dropZoneW);
  ok = ok && serialization::tryWritePod(f, dropZoneH);
  ok = ok && serialization::tryWritePod(f, static_cast<uint8_t>(hasDropZone ? 1 : 0));

  const uint32_t nSpans = static_cast<uint32_t>(spans.size());
  ok = ok && serialization::tryWritePod(f, nSpans);
  for (const GlyphSpan& sp : spans) {
    ok = ok && serialization::tryWritePod(f, sp.x);
    ok = ok && serialization::tryWritePod(f, sp.y);
    ok = ok && serialization::tryWritePod(f, static_cast<int32_t>(sp.fontId));
    ok = ok && serialization::tryWritePod(f, sp.epdStyle);
    ok = ok && serialization::tryWritePod(f, sp.dropScale);
    ok = ok && serialization::tryWriteString(f, sp.text);
  }

  const uint32_t nImgs = static_cast<uint32_t>(images.size());
  ok = ok && serialization::tryWritePod(f, nImgs);
  for (const ImagePlate& im : images) {
    ok = ok && serialization::tryWritePod(f, im.x);
    ok = ok && serialization::tryWritePod(f, im.y);
    ok = ok && serialization::tryWritePod(f, im.w);
    ok = ok && serialization::tryWritePod(f, im.h);
    ok = ok && serialization::tryWriteString(f, im.href);
  }

  const uint32_t nRules = static_cast<uint32_t>(rules.size());
  ok = ok && serialization::tryWritePod(f, nRules);
  for (const RulePlate& r : rules) {
    ok = ok && serialization::tryWritePod(f, r.x);
    ok = ok && serialization::tryWritePod(f, r.y);
    ok = ok && serialization::tryWritePod(f, r.w);
    ok = ok && serialization::tryWritePod(f, r.h);
  }

  f.close();
  if (!ok) {
    Storage.remove(writePath);
    LOG_DBG("RVPG", "save failed %s — removed", writePath);
    return false;
  }
  if (useTmp) {
    if (Storage.exists(path)) Storage.remove(path);
    if (!Storage.rename(tmpPath, path)) {
      Storage.remove(tmpPath);
      return false;
    }
  }
  return true;
}

bool LaidOutPage::loadFromFile(const char* path, const RenderKey& expectedKey, const int expectedPage) {
  clear();
  if (!path || !*path || expectedPage < 0) return false;
  HalFile f;
  if (!Storage.openFileForRead("RVPG", path, f)) return false;

  auto fail = [&](const bool corrupt) {
    f.close();
    clear();
    if (corrupt) Storage.remove(path);
    return false;
  };

  char magic[4] = {};
  if (!serialization::tryReadPod(f, magic) || std::memcmp(magic, kPageMagic, 4) != 0) {
    return fail(true);
  }
  uint16_t ver = 0;
  if (!serialization::tryReadPod(f, ver) || ver != kPageFormatVersion) {
    return fail(true);
  }
  RenderKey key{};
  if (!serialization::tryReadPod(f, key) || key != expectedKey) {
    return fail(true);
  }
  int32_t pageIndex = -1;
  if (!serialization::tryReadPod(f, pageIndex) || pageIndex != expectedPage) {
    return fail(true);
  }
  if (!readCursor(f, start) || !readCursor(f, end)) {
    return fail(true);
  }
  uint8_t endU8 = 0, dropU8 = 0;
  if (!serialization::tryReadPod(f, contentH) || !serialization::tryReadPod(f, endU8) ||
      !serialization::tryReadPod(f, dropZoneW) || !serialization::tryReadPod(f, dropZoneH) ||
      !serialization::tryReadPod(f, dropU8)) {
    return fail(true);
  }
  atChapterEnd = endU8 != 0;
  hasDropZone = dropU8 != 0;

  uint32_t nSpans = 0;
  if (!serialization::tryReadPod(f, nSpans) || nSpans > kMaxSpans) {
    LOG_ERR("RVPG", "span count %u rejected (cap %u)", static_cast<unsigned>(nSpans), static_cast<unsigned>(kMaxSpans));
    return fail(true);
  }
  {
    const uint32_t fileSize = static_cast<uint32_t>(f.size());
    const uint32_t consumed = static_cast<uint32_t>(f.position());
    const uint32_t remaining = fileSize > consumed ? fileSize - consumed : 0;
    if (static_cast<uint64_t>(nSpans) * kMinOnDiskSpanBytes > remaining) {
      LOG_ERR("RVPG", "truncated page: %u spans need %u bytes, %u left", static_cast<unsigned>(nSpans),
              static_cast<unsigned>(nSpans * kMinOnDiskSpanBytes), static_cast<unsigned>(remaining));
      return fail(true);
    }
  }
  const size_t spanBytes = static_cast<size_t>(nSpans) * sizeof(GlyphSpan);
  // SSO covers Hangul tofu spans; slack covers a few heap strings + 4KB.
  const size_t slack = static_cast<size_t>(nSpans) * 32u + 4096u;
  if (!canAlloc(spanBytes + slack)) {
    LOG_ERR("RVPG", "OOM spans=%u bytes=%u maxA=%u", static_cast<unsigned>(nSpans),
            static_cast<unsigned>(spanBytes + slack), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return fail(false);
  }
  spans.reserve(nSpans);
  for (uint32_t i = 0; i < nSpans; ++i) {
    GlyphSpan sp;
    int32_t fontId = 0;
    if (!serialization::tryReadPod(f, sp.x) || !serialization::tryReadPod(f, sp.y) ||
        !serialization::tryReadPod(f, fontId) || !serialization::tryReadPod(f, sp.epdStyle) ||
        !serialization::tryReadPod(f, sp.dropScale) || !serialization::tryReadString(f, sp.text, kMaxTextBytes)) {
      return fail(true);
    }
    sp.fontId = static_cast<int>(fontId);
    spans.push_back(std::move(sp));
  }

  uint32_t nImgs = 0;
  if (!serialization::tryReadPod(f, nImgs) || nImgs > kMaxImages) {
    return fail(true);
  }
  if (!canAlloc(static_cast<size_t>(nImgs) * sizeof(ImagePlate))) {
    return fail(false);
  }
  images.reserve(nImgs);
  for (uint32_t i = 0; i < nImgs; ++i) {
    ImagePlate im;
    if (!serialization::tryReadPod(f, im.x) || !serialization::tryReadPod(f, im.y) ||
        !serialization::tryReadPod(f, im.w) || !serialization::tryReadPod(f, im.h) ||
        !serialization::tryReadString(f, im.href, kMaxHrefBytes)) {
      return fail(true);
    }
    images.push_back(std::move(im));
  }

  uint32_t nRules = 0;
  if (!serialization::tryReadPod(f, nRules) || nRules > kMaxRules) {
    return fail(true);
  }
  if (!canAlloc(static_cast<size_t>(nRules) * sizeof(RulePlate))) {
    return fail(false);
  }
  rules.reserve(nRules);
  for (uint32_t i = 0; i < nRules; ++i) {
    RulePlate r;
    if (!serialization::tryReadPod(f, r.x) || !serialization::tryReadPod(f, r.y) ||
        !serialization::tryReadPod(f, r.w) || !serialization::tryReadPod(f, r.h)) {
      return fail(true);
    }
    rules.push_back(r);
  }

  f.close();
  return true;
}

}  // namespace rivulet
