#pragma once
// Host stub for HalStorage / HalFile.
//
// ChapterIr only touches storage in saveToFile / loadFromFile. These tests are
// about HtmlToIr parse correctness and never persist IR, so the file layer is a
// memory-backed shim: enough API surface for ChapterIr.cpp to compile and for a
// round-trip test to work without SdFat.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#ifndef O_RDONLY
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 64
#define O_TRUNC 512
#endif
#include <string>
#include <vector>

namespace storagefault { inline long reads=-1,writes=-1,seeks=-1;
inline bool fail(long& n){if(n==0)return true;if(n>0)--n;return false;}
inline void reset(){reads=writes=seeks=-1;}
}

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&& other) noexcept : buf_(std::move(other.buf_)), pos_(other.pos_) { other.pos_=0; }
  HalFile& operator=(HalFile&& other) noexcept { buf_=std::move(other.buf_); pos_=other.pos_; other.pos_=0; return *this; }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  void flush() {}
  explicit operator bool() const { return isOpen(); }
  size_t fileSize() { return size(); }


  // Memory-backed handle. openFor*() below bind buf_ to an entry in the fake FS.
  bool isOpen() const { return buf_ != nullptr; }
  void close() { buf_ = nullptr; pos_ = 0; }

  size_t size() { return buf_ ? buf_->size() : 0; }
  size_t position() const { return pos_; }
  int available() const { return buf_ ? static_cast<int>(buf_->size()-pos_) : 0; }

  bool seek(const size_t p) {
    if (storagefault::fail(storagefault::seeks) || !buf_ || p > buf_->size()) return false;
    pos_ = p;
    return true;
  }

  int read(void* dst, const size_t n) {
    if (storagefault::fail(storagefault::reads) || !buf_) return -1;
    const size_t avail = buf_->size() - pos_;
    const size_t take = n < avail ? n : avail;
    if (take > 0) std::memcpy(dst, buf_->data() + pos_, take);
    pos_ += take;
    return static_cast<int>(take);
  }

  size_t write(uint8_t b) { return write(&b, 1); }

  size_t write(const void* src, const size_t n) {
    if (storagefault::fail(storagefault::writes) || !buf_) return 0;
    if (pos_ + n > buf_->size()) buf_->resize(pos_ + n);
    if (n) std::memcpy(buf_->data()+pos_, src, n);
    pos_ += n;
    return n;
  }

 private:
  friend class HalStorageStub;
  std::shared_ptr<std::vector<uint8_t>> buf_;
  size_t pos_ = 0;
};

class HalStorageStub {
 public:
  static HalStorageStub& getInstance() {
    static HalStorageStub inst;
    return inst;
  }

  bool exists(const char* path) { return files_.count(std::string(path)) != 0; }

  bool remove(const char* path) { return files_.erase(std::string(path)) > 0; }

  bool rename(const char* from, const char* to) {
    auto it = files_.find(std::string(from));
    if (it == files_.end()) return false;
    files_[std::string(to)] = it->second;
    files_.erase(it);
    return true;
  }

  bool ensureDirectoryExists(const char*) { return true; }

  bool openFileForRead(const char*, const std::string& path, HalFile& out) {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    out.buf_ = it->second;
    out.pos_ = 0;
    return true;
  }
  bool openFileForRead(const char* tag, const char* path, HalFile& out) {
    return openFileForRead(tag, std::string(path), out);
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& out) {
    auto& buf = files_[path];
    buf = std::make_shared<std::vector<uint8_t>>();
    out.buf_ = buf;
    out.pos_ = 0;
    return true;
  }
  bool openFileForWrite(const char* tag, const char* path, HalFile& out) {
    return openFileForWrite(tag, std::string(path), out);
  }

  HalFile open(const char* path, int flags = O_RDONLY) {
    HalFile f;
    if (flags & O_TRUNC) openFileForWrite("test",path,f);
    else if (!openFileForRead("test",path,f) && (flags & O_CREAT)) openFileForWrite("test",path,f);
    return f;
  }
  void reset() { files_.clear(); }

 private:
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;
};

#define Storage HalStorageStub::getInstance()
