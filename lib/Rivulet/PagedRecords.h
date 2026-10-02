#pragma once

#include <HalStorage.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>
#include "../Memory/FallibleVector.h"

namespace rivulet {
// A bounded, write-back record window. Derived chapter data may be disk-backed;
// ordinary small host/test chapters keep the original fallible vector behavior.
// References survive only until a different window of THIS store is accessed.
// Callers retaining a block across traversal must copy that block by value.
template <class T, size_t Window = 64, size_t Max = 65534> class PagedRecords {
  static_assert(std::is_trivially_copyable<T>::value, "records must not own pointers");
 public:
  PagedRecords() = default;
  ~PagedRecords() { release(); }
  PagedRecords(const PagedRecords&) = delete;
  PagedRecords& operator=(const PagedRecords&) = delete;
  PagedRecords(PagedRecords&& o) noexcept { *this = std::move(o); }
  PagedRecords& operator=(PagedRecords&& o) noexcept {
    if (this == &o) return *this;
    release();
    mem_ = std::move(o.mem_); file_ = std::move(o.file_);
    std::memcpy(page_, o.page_, sizeof(page_));
    std::memcpy(work_, o.work_, sizeof(work_));
    count_ = o.count_; offset_ = o.offset_; pageStart_ = o.pageStart_;
    pageCount_ = o.pageCount_; disk_ = o.disk_; dirty_ = o.dirty_;
    owned_ = o.owned_; failed_ = o.failed_;
    o.count_ = o.offset_ = o.pageCount_ = 0; o.pageStart_ = SIZE_MAX;
    o.disk_ = o.dirty_ = o.owned_ = o.failed_ = false; o.work_[0] = 0;
    return *this;
  }
  bool create(const char* work) {
    release();
    if (!setWork(work)) return false;
    file_ = Storage.open(work_, O_RDWR | O_CREAT | O_TRUNC);
    if (!file_.isOpen()) { failed_ = true; return false; }
    disk_ = owned_ = true;
    return true;
  }
  bool mount(const char* path, size_t offset, size_t count, const char* work) {
    release();
    if (!setWork(work) || !Storage.openFileForRead("RVPG", path, file_)) return false;
    if (offset > file_.size() || count > (file_.size() - offset) / sizeof(T)) {
      file_.close(); failed_ = true; return false;
    }
    offset_ = offset; count_ = count; disk_ = true;
    return true;
  }
  bool detachBacking() { return writable(); }
  bool flush() const {
    if (!disk_ || !dirty_) return !failed();
    if (!owned_ || pageStart_ == SIZE_MAX || !file_.seek(offset_ + pageStart_ * sizeof(T)) ||
        file_.write(page_, pageCount_ * sizeof(T)) != pageCount_ * sizeof(T)) {
      failed_ = true; return false;
    }
    dirty_ = false;
    return !failed_;
  }
  void release() {
    // Temporary working data is never a published cache. Do not write on teardown.
    file_.close();
    if (owned_ && work_[0]) Storage.remove(work_);
    mem_.release(); count_ = offset_ = pageCount_ = 0; pageStart_ = SIZE_MAX;
    disk_ = dirty_ = owned_ = failed_ = false; work_[0] = 0;
  }
  void clear() { release(); }
  bool failed() const { return failed_ || (!disk_ && mem_.failed()); }
  size_t size() const { return disk_ ? count_ : mem_.size(); }
  bool empty() const { return size() == 0; }
  size_t capacity() const { return disk_ ? Max : mem_.capacity(); }
  bool diskBacked() const { return disk_; }
  bool reserve(size_t n) { return disk_ ? n <= Max : mem_.reserve(n); }
  bool resize(size_t n) {
    if (!disk_) return mem_.resize(n);
    if (n > Max || !writable()) return false;
    if (n < count_) {
      if (!flush()) return false;
      count_ = n; pageStart_ = SIZE_MAX; pageCount_ = 0;
      return true;
    }
    while (count_ < n) if (!push_back(T{})) return false;
    return true;
  }
  bool push_back(const T& value) {
    if (!disk_) return mem_.push_back(value);
    if (count_ >= Max || !writable()) { failed_ = true; return false; }
    if (!window(count_, true)) return false;
    page_[count_ - pageStart_] = value; ++count_;
    pageCount_ = count_ - pageStart_; dirty_ = true;
    return true;
  }
  bool append(const T* src, size_t n) {
    if (!disk_) {
      if (n > Max - std::min(Max, mem_.size()) || !mem_.reserve(mem_.size()+n)) return false;
      for (size_t i = 0; i < n; ++i) if (!mem_.push_back(src[i])) return false;
      return true;
    }
    if (n > Max - count_ || !writable()) { failed_ = true; return false; }
    while (n) {
      if (!window(count_, true)) return false;
      const size_t take = std::min(n, Window - (count_ - pageStart_));
      std::memcpy(page_ + count_ - pageStart_, src, take*sizeof(T));
      count_ += take; pageCount_ = count_ - pageStart_; dirty_ = true;
      n -= take; src += take;
    }
    return true;
  }
  bool readRange(size_t off, T* dest, size_t n) const {
    if (off > size() || n > size() - off) { failed_ = true; return false; }
    if (!disk_) { if (n) std::memcpy(dest, mem_.data()+off, n*sizeof(T)); return true; }
    while (n) {
      if (!window(off, false)) return false;
      const size_t take = std::min(n, pageCount_ - (off - pageStart_));
      if (!take) { failed_ = true; return false; }
      std::memcpy(dest, page_ + off - pageStart_, take*sizeof(T));
      off += take; dest += take; n -= take;
    }
    return true;
  }
  void pop_back() { if (size()) (void)resize(size() - 1); }
  const T& operator[](size_t i) const {
    if (!disk_) return mem_[i];
    if (i >= count_ || !window(i, false)) { failed_ = true; return emptyRecord_; }
    return page_[i - pageStart_];
  }
  T& operator[](size_t i) {
    if (!disk_) return mem_[i];
    if (i >= count_ || !writable() || !window(i, false)) { failed_ = true; return emptyRecord_; }
    dirty_ = true;
    return page_[i - pageStart_];
  }
  const T& back() const { return (*this)[size() - 1]; }
  T& back() { return (*this)[size() - 1]; }
  template <bool Const> struct Iter {
    using Owner = typename std::conditional<Const, const PagedRecords, PagedRecords>::type;
    Owner* owner; size_t index;
    auto& operator*() const { return (*owner)[index]; }
    Iter& operator++() { ++index; return *this; }
    bool operator!=(const Iter& o) const { return index != o.index; }
  };
  Iter<true> begin() const { return {this, 0}; }
  Iter<true> end() const { return {this, size()}; }
  Iter<false> begin() { return {this, 0}; }
  Iter<false> end() { return {this, size()}; }
  // Streaming export does not populate the cache or change record references.
  bool writeTo(HalFile& dest) const {
    if (!disk_) return mem_.empty() || dest.write(mem_.data(), mem_.size()*sizeof(T)) == mem_.size()*sizeof(T);
    if (!flush() || !file_.seek(offset_)) return false;
    uint8_t buf[512]; size_t left = count_ * sizeof(T);
    while (left) {
      const size_t n = std::min(left, sizeof(buf));
      if (file_.read(buf, n) != static_cast<int>(n) || dest.write(buf, n) != n) { failed_ = true; return false; }
      left -= n;
    }
    return true;
  }
 private:
  bool setWork(const char* work) {
    if (!work || !*work || std::strlen(work) >= sizeof(work_)) { failed_ = true; return false; }
    std::strcpy(work_, work); return true;
  }
  bool writable() {
    if (failed()) return false;
    if (!disk_ || owned_) return true;
    // Copy-on-write only the metadata section, never the whole chapter text.
    HalFile temp = Storage.open(work_, O_RDWR | O_CREAT | O_TRUNC);
    if (!temp.isOpen() || !writeTo(temp)) { temp.close(); Storage.remove(work_); failed_ = true; return false; }
    file_.close(); file_ = std::move(temp); offset_ = 0; owned_ = true;
    return true;
  }
  bool window(size_t i, bool append) const {
    if (failed_) return false;
    const size_t start = (i / Window) * Window;
    if (pageStart_ == start) return true;
    if (!flush()) return false;
    pageStart_ = start; pageCount_ = std::min(Window, count_ > start ? count_ - start : 0);
    std::memset(page_, 0, sizeof(page_));
    if (pageCount_ && (!file_.seek(offset_ + start*sizeof(T)) ||
        file_.read(page_, pageCount_*sizeof(T)) != static_cast<int>(pageCount_*sizeof(T)))) {
      failed_ = true; return false;
    }
    return append || pageCount_ > 0;
  }
  casper_memory::FallibleVector<T> mem_;
  mutable HalFile file_;
  mutable T page_[Window]{};
  mutable T emptyRecord_{};
  char work_[256]{};
  size_t count_ = 0, offset_ = 0;
  mutable size_t pageStart_ = SIZE_MAX, pageCount_ = 0;
  bool disk_ = false, owned_ = false;
  mutable bool dirty_ = false, failed_ = false;
};
}  // namespace rivulet
