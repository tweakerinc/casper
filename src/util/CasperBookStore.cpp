#include "CasperBookStore.h"

#include <BookPathId.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace CasperBook {
namespace {

void toLowerHex16(const uint64_t h, char out[17]) {
  static const char* kHex = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    out[i] = kHex[(h >> ((15 - i) * 4)) & 0xF];
  }
  out[16] = '\0';
}

}  // namespace

std::string normalizePath(const std::string& path) { return BookPathId::normalizePath(path); }

std::string pathIdHex(const std::string& filePath) { return BookPathId::idHex(filePath); }

std::string fileStem(const std::string& path) {
  if (path.empty()) return {};
  size_t start = path.find_last_of("/\\");
  start = (start == std::string::npos) ? 0 : start + 1;
  size_t end = path.find_last_of('.');
  if (end == std::string::npos || end < start) end = path.size();
  return path.substr(start, end - start);
}

std::string cacheFolderName(const std::string& filePath) {
  if (filePath.empty()) return {};
  const std::string h = std::to_string(std::hash<std::string>{}(filePath));
  if (FsHelpers::hasXtcExtension(filePath)) return std::string("xtc_") + h;
  if (FsHelpers::hasTxtExtension(filePath)) return std::string("txt_") + h;
  return std::string("epub_") + h;
}

std::string bookDirForPath(const std::string& filePath) {
  if (filePath.empty()) return {};
  std::string d = kRoot;
  d += "/";
  d += cacheFolderName(filePath);
  return d;
}

std::string packageDirForPath(const std::string& filePath) {
  // v0.1.8: package files live in the cache root (not a nested package/).
  return bookDirForPath(filePath);
}

std::string rivuletDirForPath(const std::string& filePath) { return bookDirForPath(filePath) + "/rivulet"; }

std::string bookDir(const std::string& idOrFolder) {
  if (idOrFolder.empty()) return {};
  // openBook returns "epub_…"; also accept full relative or legacy book_*.
  if (idOrFolder[0] == '/') return idOrFolder;
  std::string d = kRoot;
  d += "/";
  d += idOrFolder;
  return d;
}

std::string packageDir(const std::string& idOrFolder) { return bookDir(idOrFolder); }

std::string rivuletDir(const std::string& idOrFolder) { return bookDir(idOrFolder) + "/rivulet"; }

bool ensureBook(const std::string& /*idHex*/, const std::string& bookPath, const std::string& /*title*/) {
  if (bookPath.empty()) return false;
  Storage.ensureDirectoryExists(kRoot);
  const std::string dir = bookDirForPath(bookPath);
  if (dir.empty()) return false;
  Storage.ensureDirectoryExists(dir.c_str());
  Storage.ensureDirectoryExists((dir + "/rivulet").c_str());
  return true;
}

bool lookupIdByPath(const std::string& bookPath, std::string& outIdHex) {
  outIdHex.clear();
  if (bookPath.empty()) return false;
  // Folder name is the stable on-disk id for v0.1.8 layout.
  outIdHex = cacheFolderName(bookPath);
  return !outIdHex.empty();
}

bool lookupPathById(const std::string& /*idHex*/, std::string& outPath) {
  outPath.clear();
  return false;  // no ledger in v0.1.8 layout
}

void writeMeta(const std::string& /*idHex*/, const std::string& /*bookPath*/, const std::string& /*title*/,
               const std::string& /*author*/) {
  // No meta.txt required for v0.1.8 compatibility.
}

std::string openBook(const std::string& bookPath, const std::string& /*title*/, const std::string& /*author*/) {
  if (bookPath.empty()) return {};
  const std::string folder = cacheFolderName(bookPath);
  const std::string dir = bookDir(folder);
  // Each ensureDirectoryExists is a directory scan (~120ms on a full SD). Skip
  // when this session already provisioned the folder, or when rivulet is on disk.
  static std::string lastFolder;
  if (lastFolder == folder) return folder;
  const std::string rivulet = dir + "/rivulet";
  if (!Storage.exists(rivulet.c_str())) {
    Storage.ensureDirectoryExists(kRoot);
    Storage.ensureDirectoryExists(dir.c_str());
    Storage.ensureDirectoryExists(rivulet.c_str());
    LOG_DBG("CBOOK", "open cache %s", dir.c_str());
  }
  lastFolder = folder;
  return folder;
}

uint64_t hashIdentity(const char* title, const char* author, const char* stem) {
  // Kept for API compatibility only (not used for disk layout).
  std::string s;
  s.reserve(128);
  if (title) s += title;
  s.push_back('|');
  if (author) s += author;
  s.push_back('|');
  if (stem) s += stem;
  return BookPathId::fnv1a64(s);
}

std::string legacyTitleIdHex(const std::string& title, const std::string& author, const std::string& filePath) {
  const std::string stem = fileStem(filePath);
  const char* t = title.empty() ? stem.c_str() : title.c_str();
  const uint64_t h = hashIdentity(t, author.c_str(), stem.c_str());
  char hex[17];
  toLowerHex16(h, hex);
  return std::string(hex);
}

}  // namespace CasperBook
