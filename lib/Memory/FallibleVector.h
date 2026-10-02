#pragma once

// A small, move-only vector for embedded working sets. Allocation failures are
// observable, never throw/abort, and leave existing elements valid. Unlike an
// allocator probe followed by std::vector::reserve, this checks the allocation
// that is actually used. Callers must check growth, or failed() before publishing.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace casper_memory {
#ifdef CASPER_ALLOCATION_TESTING
inline long allocationsBeforeFailure = -1;
inline bool allowAllocation() {
  if (allocationsBeforeFailure < 0) return true;
  if (allocationsBeforeFailure == 0) return false;
  --allocationsBeforeFailure;
  return true;
}
#else
inline bool allowAllocation() { return true; }
#endif

template <class T> class FallibleVector {
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_default_constructible_v<T>);
  static_assert(alignof(T) <= alignof(std::max_align_t));
 public:
  FallibleVector() = default;
  ~FallibleVector() { release(); }
  FallibleVector(const FallibleVector&) = delete;
  FallibleVector& operator=(const FallibleVector&) = delete;
  FallibleVector(FallibleVector&& o) noexcept { swap(o); }
  FallibleVector& operator=(FallibleVector&& o) noexcept {
    if (this != &o) { release(); swap(o); }
    return *this;
  }
  void swap(FallibleVector& o) noexcept {
    std::swap(data_, o.data_); std::swap(size_, o.size_);
    std::swap(capacity_, o.capacity_); std::swap(failed_, o.failed_);
  }
  bool reserve(size_t n) {
    if (n <= capacity_) return true;
    if (n > std::numeric_limits<size_t>::max() / sizeof(T) || !allowAllocation()) return fail();
    if constexpr (std::is_trivially_copyable_v<T>) {
      void* p = std::realloc(data_, n * sizeof(T));
      if (!p) return fail();
      data_ = static_cast<T*>(p);
    } else {
      T* p = static_cast<T*>(std::malloc(n * sizeof(T)));
      if (!p) return fail();
      for (size_t i = 0; i < size_; ++i) {
        new (p + i) T(std::move(data_[i]));
        data_[i].~T();
      }
      std::free(data_);
      data_ = p;
    }
    capacity_ = n;
    return true;
  }
  bool resize(size_t n) {
    if (n > capacity_ && !reserve(n)) return false;
    while (size_ > n) pop_back();
    while (size_ < n) { new (data_ + size_) T(); ++size_; }
    return true;
  }
  bool push_back(const T& value) {
    // Store a copy first because value may alias an element invalidated by grow.
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    T copy(value);
    return push_back(std::move(copy));
  }
  bool push_back(T&& value) {
    T moved(std::move(value));
    if (size_ == capacity_) {
      const size_t extra = std::max<size_t>(8, capacity_ / 2);
      if (capacity_ > std::numeric_limits<size_t>::max() - extra || !reserve(capacity_ + extra)) return fail();
    }
    new (data_ + size_) T(std::move(moved)); ++size_; return true;
  }
  template<class It> bool assign(It first, It last) {
    clear();
    for (; first != last; ++first) if (!push_back(*first)) return false;
    return true;
  }
  void erasePrefix(size_t n) {
    n = std::min(n, size_);
    for (size_t i = n; i < size_; ++i) data_[i - n] = std::move(data_[i]);
    const size_t target = size_ - n;
    while (size_ > target) pop_back();
  }
  void pop_back() { if (size_) data_[--size_].~T(); }
  void clear() { while (size_) pop_back(); failed_ = false; }
  void release() { clear(); std::free(data_); data_ = nullptr; capacity_ = 0; }
  bool failed() const { return failed_; }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  T* data() { return data_; }
  const T* data() const { return data_; }
  T* begin() { return data_; }
  const T* begin() const { return data_; }
  T* end() { return size_ ? data_ + size_ : data_; }
  const T* end() const { return size_ ? data_ + size_ : data_; }
  T& operator[](size_t n) { return data_[n]; }
  const T& operator[](size_t n) const { return data_[n]; }
  T& back() { return data_[size_ - 1]; }
  const T& back() const { return data_[size_ - 1]; }
  T& front() { return data_[0]; }
  const T& front() const { return data_[0]; }
 private:
  bool fail() { failed_ = true; return false; }
  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  bool failed_ = false;
};
}  // namespace casper_memory
