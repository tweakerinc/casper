#pragma once

#include <FallibleVector.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Navigation documents describe logical chapters; OPF spine files describe
// reading order. Never synthesize chapters just because these counts differ.
namespace epubnav {
class Targets {
  struct Entry {
    uint32_t offset;
    uint16_t pathSize;
    uint16_t anchorSize;
    Entry() noexcept : offset(0), pathSize(0), anchorSize(0) {}
  };
  casper_memory::FallibleVector<Entry> entries_;
  casper_memory::FallibleVector<char> bytes_;
  bool valid_ = true;
  static constexpr size_t kMaxEntries = 2048;
  static constexpr size_t kMaxBytes = 32768;

 public:
  void add(const std::string& path, const std::string& anchor) {
    if (!valid_) return;
    const size_t n = path.size() + anchor.size();
    if (path.empty() || path.size() > 4096 || anchor.size() > 4096 ||
        path.find("://") != std::string::npos || entries_.size() >= kMaxEntries ||
        n > kMaxBytes - bytes_.size()) { valid_ = false; return; }
    Entry e;
    e.offset = static_cast<uint32_t>(bytes_.size());
    e.pathSize = static_cast<uint16_t>(path.size());
    e.anchorSize = static_cast<uint16_t>(anchor.size());
    const size_t required = bytes_.size() + n;
    // Geometric growth avoids reallocating for every streamed entry.
    if (required > bytes_.capacity() &&
        !bytes_.reserve(std::min(kMaxBytes, std::max(required, bytes_.capacity() * 2 + 512)))) {
      valid_ = false; return;
    }
    if (!bytes_.resize(required) || !entries_.push_back(e)) { valid_ = false; return; }
    std::memcpy(bytes_.data() + e.offset, path.data(), path.size());
    std::memcpy(bytes_.data() + e.offset + e.pathSize, anchor.data(), anchor.size());
  }
  bool valid() const { return valid_ && !entries_.failed() && !bytes_.failed(); }
  size_t size() const { return entries_.size(); }
  bool equal(size_t a, const Targets& other, size_t b) const {
    if (a >= size() || b >= other.size()) return false;
    const auto& x = entries_[a]; const auto& y = other.entries_[b];
    return x.pathSize == y.pathSize && x.anchorSize == y.anchorSize &&
           std::memcmp(bytes_.data()+x.offset, other.bytes_.data()+y.offset,
                       size_t(x.pathSize)+x.anchorSize) == 0;
  }
  size_t uniqueCount() const {
    size_t n = 0;
    for (size_t i=0; i<size(); ++i) {
      bool seen=false;
      for (size_t j=0; j<i; ++j) if (equal(i,*this,j)) { seen=true; break; }
      if (!seen) ++n;
    }
    return n;
  }
};

// Prefer EPUB 3 navigation except when the NCX is a verified extension: every
// original target (including fragment) is retained in order, with additional
// distinct targets. Mere entry count, spine count or duplicate links is not proof.
inline bool preferNcx(const Targets& nav, bool navParsed, const Targets& ncx, bool ncxParsed) {
  if (!ncxParsed || !ncx.valid() || !ncx.size()) return false;
  if (!navParsed) return true;
  if (!nav.valid()) return false;
  if (!nav.size()) return true;
  if (ncx.uniqueCount() <= nav.uniqueCount()) return false;
  size_t matched=0;
  for (size_t i=0; i<ncx.size() && matched<nav.size(); ++i)
    if (nav.equal(matched,ncx,i)) ++matched;
  return matched == nav.size();
}
inline bool useSpineFallback(int tocCount) { return tocCount == 0; }
}  // namespace epubnav
