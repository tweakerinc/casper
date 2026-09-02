#include "BookPathId.h"

#include <FsHelpers.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace BookPathId {
namespace {

void toLowerHex16(const uint64_t h, char out[17]) {
  static const char* kHex = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    out[i] = kHex[(h >> ((15 - i) * 4)) & 0xF];
  }
  out[16] = '\0';
}

}  // namespace

std::string normalizePath(const std::string& path) {
  if (path.empty()) return {};

  // Unify separators.
  std::string s;
  s.reserve(path.size() + 1);
  for (char c : path) {
    s.push_back(c == '\\' ? '/' : c);
  }

  // Strip Windows drive "C:" / "C:/"
  if (s.size() >= 2 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':') {
    s.erase(0, 2);
  }

  // Strip common volume prefixes if present.
  // (Device paths are already absolute from SD root.)
  while (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
    s.erase(0, 1);
  }

  // Collapse . / .. via existing helper (expects '/' separators; no leading required).
  // Work on path without leading slash for component logic, then re-add.
  std::string body = s;
  while (!body.empty() && body[0] == '/') body.erase(0, 1);
  // Also strip a trailing slash (except empty).
  while (body.size() > 1 && body.back() == '/') body.pop_back();

  std::string collapsed = FsHelpers::normalisePath(body);
  if (collapsed.empty()) {
    // Root-only or empty after collapse.
    if (s == "/" || path == "/" || path == "\\") return "/";
    return {};
  }

  std::string out;
  out.reserve(collapsed.size() + 1);
  out.push_back('/');
  out += collapsed;
  return out;
}

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string idHex(const std::string& path) {
  const std::string norm = normalizePath(path);
  const uint64_t h = fnv1a64(norm);
  char hex[17];
  toLowerHex16(h, hex);
  return std::string(hex);
}

std::string bookRoot(const std::string& path, const char* casperRoot) {
  std::string d = casperRoot ? casperRoot : kCasperRoot;
  d += "/book_";
  d += idHex(path);
  return d;
}

std::string packageDir(const std::string& path, const char* casperRoot) {
  return bookRoot(path, casperRoot) + "/package";
}

std::string rivuletDir(const std::string& path, const char* casperRoot) {
  return bookRoot(path, casperRoot) + "/rivulet";
}

bool isCasperPackageRoot(const std::string& cacheDir) {
  if (cacheDir.empty()) return false;
  // Exact or trailing match for "/.crosspoint"
  if (cacheDir == kCasperRoot) return true;
  if (cacheDir == "/.crosspoint/") return true;
  static constexpr const char kTail[] = "/.crosspoint";
  constexpr size_t kTailLen = sizeof(kTail) - 1;
  return cacheDir.size() >= kTailLen &&
         (cacheDir.compare(cacheDir.size() - kTailLen, kTailLen, kTail) == 0 ||
          cacheDir == ".crosspoint");
}

std::string legacyEpubHashDir(const std::string& path, const char* root) {
  std::string d = root ? root : kCasperRoot;
  d += "/epub_";
  d += std::to_string(std::hash<std::string>{}(path));
  return d;
}

}  // namespace BookPathId
