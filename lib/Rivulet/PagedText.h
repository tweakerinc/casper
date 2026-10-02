#pragma once
#include "PagedRecords.h"
namespace rivulet {
// Immutable source text plus an append-only edit tail. Re-resolving an image
// href must not copy a megabyte of prose just to append a few path bytes.
class PagedText {
 public:
  bool create(const char* path) { baseSize_=0; base_.close(); return tail_.create(path); }
  bool mount(const char* path, size_t off, size_t size, const char* work) {
    if (std::strlen(work) >= sizeof(work_) || !Storage.openFileForRead("RVTEXT",path,base_)) return false;
    std::strcpy(work_,work); baseOffset_=off; baseSize_=size;
    return off <= base_.size() && size <= base_.size()-off;
  }
  bool append(const char* p, size_t len) {
    if (!tail_.diskBacked() && !tail_.create(work_)) { failed_=true; return false; }
    return tail_.append(p,len);
  }
  bool readRange(size_t off, char* p, size_t len) const {
    if (off > size() || len > size()-off) { failed_=true; return false; }
    if (off < baseSize_) {
      const size_t n=std::min(len,baseSize_-off);
      if (!base_.seek(baseOffset_+off) || base_.read(p,n)!=static_cast<int>(n)) { failed_=true; return false; }
      off+=n; p+=n; len-=n;
    }
    return !len || tail_.readRange(off-baseSize_,p,len);
  }
  bool writeTo(HalFile& dest) const {
    if (baseSize_) {
      char buf[512]; size_t left=baseSize_;
      if (!base_.seek(baseOffset_)) return false;
      while (left) {
        const size_t n=std::min(left,sizeof(buf));
        if (base_.read(buf,n)!=static_cast<int>(n) || dest.write(buf,n)!=n) { failed_=true; return false; }
        left-=n;
      }
    }
    return !failed() && tail_.writeTo(dest);
  }
  size_t size() const { return baseSize_+tail_.size(); }
  bool failed() const { return failed_||tail_.failed(); }
 private:
  mutable HalFile base_;
  size_t baseOffset_=0,baseSize_=0;
  PagedRecords<char,4096,32U*1024U*1024U> tail_;
  char work_[256]{};
  mutable bool failed_=false;
};
} // namespace rivulet
