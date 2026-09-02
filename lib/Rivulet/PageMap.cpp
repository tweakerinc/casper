#include "PageMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

namespace rivulet {

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
  HalFile f;
  if (!Storage.openFileForWrite("RVPM", path, f)) return false;
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
  if (!ok) Storage.remove(path);
  return ok;
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
  if (!serialization::tryReadPod(f, ver) || ver != kMapFormatVersion) {
    f.close();
    return false;
  }
  if (!serialization::tryReadPod(f, key_)) {
    f.close();
    return false;
  }
  uint32_t n = 0;
  if (!serialization::tryReadPod(f, n) || n > 100000) {
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
