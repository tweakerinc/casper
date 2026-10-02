#pragma once

#include <cstdint>
#include <string>

// Path helpers. Product on-disk layout matches shipped CrossPoint v0.1.8:
//   /.crosspoint/epub_<std::hash(path)>/   package + progress + stats
//   /.crosspoint/epub_<hash>/rivulet/      Rivulet IR only (additive)
//
// FNV path ids / book_<id> helpers remain for optional tooling and cleaning
// accidental WIP folders — firmware does not rekey or require them.
//
// normalizePath: strip drive letters / volume roots; '\' → '/'; leading '/'.

namespace BookPathId {

inline constexpr const char* kCrossPointRoot = "/.crosspoint";
inline constexpr size_t kIdHexLen = 16;

// Shared normalize: ignore H: vs device mount — path of the book on the SD only.
std::string normalizePath(const std::string& path);

// FNV-1a 64 over raw bytes (not null-terminated special fields).
uint64_t fnv1a64(const std::string& s);

// 16 lowercase hex digits of FNV(normalize(path)).
std::string idHex(const std::string& path);

// /.crosspoint/book_<id>
std::string bookRoot(const std::string& path, const char* casperRoot = kCrossPointRoot);

// /.crosspoint/book_<id>/package
std::string packageDir(const std::string& path, const char* casperRoot = kCrossPointRoot);

// /.crosspoint/book_<id>/rivulet
std::string rivuletDir(const std::string& path, const char* casperRoot = kCrossPointRoot);

// True when cacheDir is the package root (use unified book_ layout).
bool isCrossPointPackageRoot(const std::string& cacheDir);

// Legacy CrossPoint/legacy package dir using std::hash (import-only).
// Only valid on-device (same std::hash as firmware that wrote the cache).
std::string legacyEpubHashDir(const std::string& path, const char* root);

}  // namespace BookPathId
