#include "PageMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>

namespace rivulet {
namespace {
// Sanity cap for a single chapter's page count (see loadFromFile).
constexpr uint32_t kMaxMapPages = 1000000;
uint32_t mapWorkSerial = 0;
}  // namespace

bool PageMap::enablePaging(const char* basePath) {
  if (!basePath || !*basePath || std::strlen(basePath)>=sizeof(pagingBase_)) return false;
  std::strcpy(pagingBase_,basePath);
  char work[256];std::snprintf(work,sizeof(work),"%s.pm%lu",basePath,static_cast<unsigned long>(++mapWorkSerial));
  return starts_.create(work);
}

void PageMap::clear() {
  starts_.release();
  complete_ = false;
  knownTotal_ = 0;
  key_ = {};
  pagingBase_[0]=0;
}

bool PageMap::resetWithStart(const IrCursor& firstPageStart) {
  if (starts_.diskBacked()) { if (!starts_.resize(0)) return false; } else starts_.clear();
  const bool ok = starts_.push_back(firstPageStart);
  complete_ = false;
  knownTotal_ = 0;
  return ok;
}

bool PageMap::pushPageStart(const IrCursor& c) {
  if (!starts_.push_back(c)) { markIncomplete(); return false; }
  // Extending past a "complete" map means that total was wrong (stale .rvpm or
  // false end==start complete). Drop complete so counts/idle walk resume.
  if (complete_ && static_cast<int>(starts_.size()) > knownTotal_) {
    complete_ = false;
    knownTotal_ = 0;
  }
  return true;
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

bool PageMap::setPageStart(const int pageIndex, const IrCursor& c) {
  if (pageIndex < 0) return false;
  if (pageIndex > static_cast<int>(starts_.size())) {
    // Cannot leave holes — only extend by one at a time via pushPageStart.
    return false;
  }
  if (pageIndex == static_cast<int>(starts_.size())) {
    if (!starts_.push_back(c)) { markIncomplete(); return false; }
  } else {
    starts_[static_cast<size_t>(pageIndex)] = c;
    // Later starts are no longer valid relative to this re-break.
    if (pageIndex + 1 < static_cast<int>(starts_.size())) {
      starts_.resize(static_cast<size_t>(pageIndex + 1));
    }
  }
  complete_ = false;
  knownTotal_ = 0;
  return !failed();
}

IrCursor PageMap::pageStart(const int pageIndex) const {
  if (!hasPage(pageIndex)) return {};
  return starts_[static_cast<size_t>(pageIndex)];
}

bool PageMap::saveToFile(const char* path) const {
  if (failed() || !path || !*path || !starts_.detachBacking()) return false;
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
  static_assert(sizeof(IrCursor)==6);
  ok = ok && starts_.writeTo(f);
  f.flush();
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
  if (completeU8 > 1 || knownTotal_ < 0 || (completeU8 && knownTotal_ != static_cast<int>(n)) ||
      static_cast<uint64_t>(n) * kCursorBytes != remaining) { clear(); f.close(); return false; }
  char work[256];
  if (std::snprintf(work,sizeof(work),"%s.pm%lu",path,static_cast<unsigned long>(++mapWorkSerial))>=int(sizeof(work))) return false;
  f.close();
  if (!starts_.mount(path,consumed,n,work)) { clear(); return false; }
  const auto& records=static_cast<const decltype(starts_)&>(starts_);
  IrCursor previous{};
  for (uint32_t i=0;i<n;++i) {
    const IrCursor current=records[i];
    if (records.failed() || (i && !(previous<current))) { clear(); return false; }
    previous=current;
  }
  complete_ = completeU8 != 0;
  f.close();
  return true;
}

}  // namespace rivulet
