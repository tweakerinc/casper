#pragma once

#include <HalStorage.h>

#include <initializer_list>
#include <string>

#include "ThumbCachePolicy.h"

// Shared SD probe for Bare Home blit + Home multipass. exists() alone is not
// enough: a 70-byte header leftover or a 0-byte file would "find" a jacket
// that then fails to parse and paints the white placeholder forever.
namespace coverthumb {

inline bool fileIsValidBmp(const char* tag, const std::string& path) {
  if (path.empty()) return false;
  HalFile probe;
  if (!Storage.openFileForRead(tag, path, probe)) return false;
  char sig[2] = {};
  const size_t n = probe.read(sig, 2);
  const size_t sz = probe.size();
  probe.close();
  return thumbcache::validBmpProbe(static_cast<unsigned>(n), sig[0], sig[1], static_cast<unsigned>(sz));
}

inline bool fileLooksPresent(const char* tag, const std::string& path) {
  if (path.empty()) return false;
  HalFile probe;
  if (Storage.openFileForRead(tag, path, probe)) {
    probe.close();
    return true;
  }
  return Storage.exists(path.c_str());
}

inline std::string firstValidBmp(const char* tag, std::initializer_list<std::string> candidates) {
  for (const std::string& path : candidates) {
    if (fileIsValidBmp(tag, path)) return path;
  }
  return {};
}

}  // namespace coverthumb
