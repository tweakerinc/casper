#pragma once
#include "FallibleVector.h"
#include <cstring>
#include <string_view>

namespace casper_memory {
// Owned UTF-8 bytes for page snapshots. The normal small-word path does not
// allocate. Allocation failure preserves old data and must reject the snapshot.
class FallibleString {
 public:
  FallibleString() noexcept = default;
  ~FallibleString() { std::free(heap_); }
  FallibleString(const FallibleString& o) noexcept { assign(o.data(), o.size()); failed_ = failed_ || o.failed_; }
  FallibleString(FallibleString&& o) noexcept { moveFrom(o); }
  FallibleString& operator=(FallibleString&& o) noexcept {
    if (this != &o) { std::free(heap_); heap_ = nullptr; moveFrom(o); }
    return *this;
  }
  FallibleString& operator=(const FallibleString& o) noexcept {
    if (this != &o) { assign(o.data(), o.size()); failed_ = failed_ || o.failed_; }
    return *this;
  }
  FallibleString& operator=(std::string_view s) noexcept { assign(s.data(), s.size()); return *this; }
  bool assign(const char* s, size_t n) noexcept {
    if (!s && n) { failed_ = true; return false; }
    // Grow through a new block because s may point into this string.
    if (n > capacity_) {
      if (n == std::numeric_limits<size_t>::max() || !allowAllocation()) { failed_ = true; return false; }
      char* p = static_cast<char*>(std::malloc(n + 1));
      if (!p) { failed_ = true; return false; }
      if (n) std::memcpy(p, s, n);
      p[n] = 0;
      std::free(heap_); heap_ = p; capacity_ = n;
    } else {
      if (n) std::memmove(data(), s, n);
      data()[n] = 0;
    }
    length_ = n;
    return true;
  }
  bool resize(size_t n) noexcept {
    if (n > capacity_) {
      if (n == std::numeric_limits<size_t>::max() || !allowAllocation()) { failed_ = true; return false; }
      char* p = static_cast<char*>(std::malloc(n + 1));
      if (!p) { failed_ = true; return false; }
      std::memcpy(p, data(), length_);
      std::free(heap_); heap_ = p; capacity_ = n;
    }
    if (n > length_) std::memset(data() + length_, 0, n - length_);
    data()[n] = 0; length_ = n; return true;
  }
  size_t capacity() const noexcept { return capacity_; }
  bool reserve(size_t n) noexcept {
    if(n<=capacity_)return !failed_;
    if(n==std::numeric_limits<size_t>::max() || !allowAllocation()){failed_=true;return false;}
    char* p=static_cast<char*>(std::malloc(n+1));
    if(!p){failed_=true;return false;}
    std::memcpy(p,data(),length_+1);std::free(heap_);heap_=p;capacity_=n;return true;
  }
  bool append(std::string_view s) noexcept {
    if(s.size()>std::numeric_limits<size_t>::max()-length_-1){failed_=true;return false;}
    const size_t size=length_+s.size();
    if(size>capacity_){
      if(!allowAllocation()){failed_=true;return false;}
      char* p=static_cast<char*>(std::malloc(size+1));if(!p){failed_=true;return false;}
      std::memcpy(p,data(),length_);if(!s.empty())std::memcpy(p+length_,s.data(),s.size());p[size]=0;
      std::free(heap_);heap_=p;capacity_=size;
    }else{if(!s.empty())std::memmove(data()+length_,s.data(),s.size());data()[size]=0;}
    length_=size;return !failed_;
  }
  bool push_back(char c) noexcept { return append(std::string_view(&c,1)); }
  void pop_back() noexcept { if(length_)data()[--length_]=0; }
  char& operator[](size_t i) noexcept { return data()[i]; }
  const char& operator[](size_t i) const noexcept { return data()[i]; }
  char& back() noexcept { return data()[length_-1]; }
  const char& back() const noexcept { return data()[length_-1]; }
  FallibleString& operator+=(std::string_view s) noexcept {append(s);return *this;}
  FallibleString& operator+=(const FallibleString& s) noexcept {append(s.view());return *this;}
  void clear() noexcept { length_ = 0; data()[0] = 0; failed_ = false; }
  bool empty() const noexcept { return length_ == 0; }
  bool failed() const noexcept { return failed_; }
  size_t size() const noexcept { return length_; }
  char* data() noexcept { return heap_ ? heap_ : small_; }
  const char* data() const noexcept { return heap_ ? heap_ : small_; }
  const char* c_str() const noexcept { return data(); }
  size_t find(std::string_view needle, size_t pos = 0) const noexcept { return view().find(needle, pos); }
  std::string_view view() const noexcept { return {data(), length_}; }
  bool operator==(std::string_view s) const noexcept { return view() == s; }
  bool operator==(const FallibleString& s) const noexcept { return view() == s.view(); }
 private:
  void moveFrom(FallibleString& o) noexcept {
    heap_ = o.heap_; length_ = o.length_; capacity_ = o.capacity_; failed_ = o.failed_;
    if (!heap_) std::memcpy(small_, o.small_, sizeof(small_));
    o.heap_ = nullptr; o.length_ = 0; o.capacity_ = sizeof(small_) - 1; o.small_[0] = 0; o.failed_ = false;
  }
  char* heap_ = nullptr;
  size_t length_ = 0;
  size_t capacity_ = 15;
  char small_[16] = {};
  bool failed_ = false;
};
}  // namespace casper_memory
