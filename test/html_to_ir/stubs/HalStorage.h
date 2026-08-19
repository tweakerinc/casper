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
#include <string>
#include <vector>

class HalFile {
 public:
  HalFile() = default;

  // Memory-backed handle. openFor*() below bind buf_ to an entry in the fake FS.
  bool isOpen() const { return buf_ != nullptr; }
  void close() { buf_ = nullptr; pos_ = 0; }

  size_t size() { return buf_ ? buf_->size() : 0; }
  size_t position() const { return pos_; }

  bool seek(const size_t p) {
    if (!buf_ || p > buf_->size()) return false;
    pos_ = p;
    return true;
  }

  int read(void* dst, const size_t n) {
    if (!buf_) return -1;
    const size_t avail = buf_->size() - pos_;
    const size_t take = n < avail ? n : avail;
    if (take > 0) std::memcpy(dst, buf_->data() + pos_, take);
    pos_ += take;
    return static_cast<int>(take);
  }

  size_t write(const void* src, const size_t n) {
    if (!buf_) return 0;
    const auto* p = static_cast<const uint8_t*>(src);
    buf_->insert(buf_->end(), p, p + n);
    pos_ = buf_->size();
    return n;
  }

 private:
  friend class HalStorageStub;
  std::vector<uint8_t>* buf_ = nullptr;
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
    out.buf_ = &it->second;
    out.pos_ = 0;
    return true;
  }
  bool openFileForRead(const char* tag, const char* path, HalFile& out) {
    return openFileForRead(tag, std::string(path), out);
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& out) {
    auto& buf = files_[path];
    buf.clear();
    out.buf_ = &buf;
    out.pos_ = 0;
    return true;
  }
  bool openFileForWrite(const char* tag, const char* path, HalFile& out) {
    return openFileForWrite(tag, std::string(path), out);
  }

  void reset() { files_.clear(); }

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
};

#define Storage HalStorageStub::getInstance()
