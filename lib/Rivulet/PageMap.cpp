#include "PageMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>

namespace rivulet {
namespace {
// Sanity cap for a single chapter's page count (see loadFromFile).
constexpr uint32_t kMaxMapPages = 4000;
}  // namespace

void PageMap::clear() {
  starts_.clear();
  complete_ = false;
  knownTotal_ = 0;
  key_ = {};
}

void PageMap::resetWithStart(const IrCursor& firstPageStart) {
  starts_.clear();
  starts_.push_back(firstPageStart);
  complete_ = false;
  knownTotal_ = 0;
}

void PageMap::pushPageStart(const IrCursor& c) {
  starts_.push_back(c);
  // Extending past a "complete" map means that total was wrong (stale .rvpm or
  // false end==start complete). Drop complete so counts/idle walk resume.
  if (complete_ && static_cast<int>(starts_.size()) > knownTotal_) {
    complete_ = false;
    knownTotal_ = 0;
  }
}

void PageMap::truncateFrom(const int pageIndex) {
  if (pageIndex < 0) {
    starts_.clear();
  } else if (pageIndex < static_cast<int>(starts_.size())) {
    starts_.resize(static_cast<size_t>(pageIndex));
  }
  complete_ = false;
  knownTotal_ = 0;
}

void PageMap::setPageStart(const int pageIndex, const IrCursor& c) {
  if (pageIndex < 0) return;
  if (pageIndex > static_cast<int>(starts_.size())) {
    // Cannot leave holes — only extend by one at a time via pushPageStart.
    return;
  }
  if (pageIndex == static_cast<int>(starts_.size())) {
    starts_.push_back(c);
  } else {
    starts_[static_cast<size_t>(pageIndex)] = c;
    // Later starts are no longer valid relative to this re-break.
    if (pageIndex + 1 < static_cast<int>(starts_.size())) {
      starts_.resize(static_cast<size_t>(pageIndex + 1));
    }
  }
  complete_ = false;
  knownTotal_ = 0;
}

IrCursor PageMap::pageStart(const int pageIndex) const {
  if (!hasPage(pageIndex)) return {};
  return starts_[static_cast<size_t>(pageIndex)];
}

bool PageMap::saveToFile(const char* path) const {
  if (!path || !*path) return false;
  // Atomic: write .tmp then rename. A power loss mid-write used to leave a
  // corrupt .rvpm at the final path, which the loader then had to defend against.
  char tmpPath[224];
  const int wrote = std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  const bool useTmp = wrote > 0 && static_cast<size_t>(wrote) < sizeof(tmpPath);
  const char* writePath = useTmp ? tmpPath : path;
  if (useTmp && Storage.exists(tmpPath)) Storage.remove(tmpPath);
  HalFile f;
  if (!Storage.openFileForWrite("RVPM", writePath, f)) return false;
  bool ok = serialization::tryWritePod(f, kMapMagic);
  ok = ok && serialization::tryWritePod(f, kMapFormatVersion);
  ok = ok && serialization::tryWritePod(f, key_);
  const uint32_t n = static_cast<uint32_t>(starts_.size());
  ok = ok && serialization::tryWritePod(f, n);
  const uint8_t completeU8 = complete_ ? 1 : 0;
  ok = ok && serialization::tryWritePod(f, completeU8);
  ok = ok && serialization::tryWritePod(f, knownTotal_);
  for (const IrCursor& c : starts_) {
    ok = ok && serialization::tryWritePod(f, c.blockIndex);
    ok = ok && serialization::tryWritePod(f, c.runIndex);
    ok = ok && serialization::tryWritePod(f, c.byteInRun);
  }
  f.close();
  if (!ok) {
    Storage.remove(writePath);
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

bool PageMap::loadFromFile(const char* path) {
  clear();
  if (!path || !*path) return false;
  HalFile f;
  if (!Storage.openFileForRead("RVPM", path, f)) return false;
  char magic[4] = {};
  if (!serialization::tryReadPod(f, magic) || std::memcmp(magic, kMapMagic, 4) != 0) {
    f.close();
    return false;
  }
  uint16_t ver = 0;
  if (!serialization::tryReadPod(f, ver) || ver < kMapFormatVersionMin || ver > kMapFormatVersion) {
    f.close();
    return false;
  }
  if (!serialization::tryReadPod(f, key_)) {
    f.close();
    return false;
  }
  uint32_t n = 0;
  // Hard cap: a single chapter never has 100k pages, and resize(n) allocates
  // n * sizeof(IrCursor) BEFORE any cursor is read. With -fno-exceptions a failed
  // vector allocation calls abort(), so a corrupt header used to crash the device
  // (600 KB request on a 380 KB part). Cap by sanity AND by bytes actually left.
  if (!serialization::tryReadPod(f, n) || n > kMaxMapPages) {
    LOG_ERR("RVPM", "page count %u rejected (cap %u)", static_cast<unsigned>(n), static_cast<unsigned>(kMaxMapPages));
    f.close();
    return false;
  }
  uint8_t completeU8 = 0;
  if (!serialization::tryReadPod(f, completeU8)) {
    f.close();
    return false;
  }
  if (!serialization::tryReadPod(f, knownTotal_)) {
    f.close();
    return false;
  }
  // Each cursor is 3 × uint16_t on disk; refuse a count the file cannot hold.
  constexpr uint32_t kCursorBytes = 3 * sizeof(uint16_t);
  const uint32_t fileSize = static_cast<uint32_t>(f.size());
  const uint32_t consumed = static_cast<uint32_t>(f.position());
  const uint32_t remaining = fileSize > consumed ? fileSize - consumed : 0;
  if (static_cast<uint64_t>(n) * kCursorBytes > remaining) {
    LOG_ERR("RVPM", "truncated map: %u pages need %u bytes, %u left", static_cast<unsigned>(n),
            static_cast<unsigned>(n * kCursorBytes), static_cast<unsigned>(remaining));
    f.close();
    return false;
  }
  starts_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!serialization::tryReadPod(f, starts_[i].blockIndex) || !serialization::tryReadPod(f, starts_[i].runIndex) ||
        !serialization::tryReadPod(f, starts_[i].byteInRun)) {
      clear();
      f.close();
      return false;
    }
  }
  complete_ = completeU8 != 0;
  f.close();
  return true;
}

}  // namespace rivulet
