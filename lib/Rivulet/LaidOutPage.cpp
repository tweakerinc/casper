#include "LaidOutPage.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>

namespace rivulet {
namespace {

constexpr char kPageMagic[4] = {'R', 'V', 'P', 'G'};
// v2: page carries drawn thematic-break rules (RulePlate) as well as spans/images.
constexpr uint16_t kPageFormatVersion = 2;
// Soft caps — a pathological page should not allocate unbounded on load.
constexpr uint32_t kMaxSpans = 2000;
constexpr uint32_t kMaxImages = 64;
constexpr uint32_t kMaxRules = 64;
constexpr uint32_t kMaxTextBytes = 4096;

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

  char magic[4] = {};
  if (!serialization::tryReadPod(f, magic) || std::memcmp(magic, kPageMagic, 4) != 0) {
    f.close();
    return false;
  }
  uint16_t ver = 0;
  if (!serialization::tryReadPod(f, ver) || ver != kPageFormatVersion) {
    f.close();
    return false;
  }
  RenderKey key{};
  if (!serialization::tryReadPod(f, key) || key != expectedKey) {
    f.close();
    return false;
  }
  int32_t pageIndex = -1;
  if (!serialization::tryReadPod(f, pageIndex) || pageIndex != expectedPage) {
    f.close();
    return false;
  }
  if (!readCursor(f, start) || !readCursor(f, end)) {
    f.close();
    clear();
    return false;
  }
  uint8_t endU8 = 0, dropU8 = 0;
  if (!serialization::tryReadPod(f, contentH) || !serialization::tryReadPod(f, endU8) ||
      !serialization::tryReadPod(f, dropZoneW) || !serialization::tryReadPod(f, dropZoneH) ||
      !serialization::tryReadPod(f, dropU8)) {
    f.close();
    clear();
    return false;
  }
  atChapterEnd = endU8 != 0;
  hasDropZone = dropU8 != 0;

  uint32_t nSpans = 0;
  if (!serialization::tryReadPod(f, nSpans) || nSpans > kMaxSpans) {
    f.close();
    clear();
    return false;
  }
  spans.reserve(nSpans);
  for (uint32_t i = 0; i < nSpans; ++i) {
    GlyphSpan sp;
    int32_t fontId = 0;
    if (!serialization::tryReadPod(f, sp.x) || !serialization::tryReadPod(f, sp.y) ||
        !serialization::tryReadPod(f, fontId) || !serialization::tryReadPod(f, sp.epdStyle) ||
        !serialization::tryReadPod(f, sp.dropScale) || !serialization::tryReadString(f, sp.text)) {
      f.close();
      clear();
      return false;
    }
    if (sp.text.size() > kMaxTextBytes) {
      f.close();
      clear();
      return false;
    }
    sp.fontId = static_cast<int>(fontId);
    spans.push_back(std::move(sp));
  }

  uint32_t nImgs = 0;
  if (!serialization::tryReadPod(f, nImgs) || nImgs > kMaxImages) {
    f.close();
    clear();
    return false;
  }
  images.reserve(nImgs);
  for (uint32_t i = 0; i < nImgs; ++i) {
    ImagePlate im;
    if (!serialization::tryReadPod(f, im.x) || !serialization::tryReadPod(f, im.y) ||
        !serialization::tryReadPod(f, im.w) || !serialization::tryReadPod(f, im.h) ||
        !serialization::tryReadString(f, im.href)) {
      f.close();
      clear();
      return false;
    }
    images.push_back(std::move(im));
  }

  uint32_t nRules = 0;
  if (!serialization::tryReadPod(f, nRules) || nRules > kMaxRules) {
    f.close();
    clear();
    return false;
  }
  rules.reserve(nRules);
  for (uint32_t i = 0; i < nRules; ++i) {
    RulePlate r;
    if (!serialization::tryReadPod(f, r.x) || !serialization::tryReadPod(f, r.y) ||
        !serialization::tryReadPod(f, r.w) || !serialization::tryReadPod(f, r.h)) {
      f.close();
      clear();
      return false;
    }
    rules.push_back(r);
  }

  f.close();
  return true;
}

}  // namespace rivulet
