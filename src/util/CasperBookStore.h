#pragma once

#include <cstdint>
#include <string>

// Book cache paths — same layout as shipped Casper v0.1.8.
//
// Under /.crosspoint:
//   epub_<std::hash(path)>/   (or xtc_ / txt_)
//     book.bin, thumbs, sections, progress.bin, stats_*.bin
//     rivulet/                 # Rivulet IR only (additive; not present in 0.1.8)
//
// No book_<fnv> rekey and no path-layout migration. Upgrades keep existing folders.

namespace CasperBook {

inline constexpr const char* kRoot = "/.crosspoint";
inline constexpr const char* kLedgerName = "ledger.tsv";  // unused for layout; kept for API compat
inline constexpr size_t kIdHexLen = 16;

// Path normalize (shared with BookPathId if needed for other tools).
std::string normalizePath(const std::string& path);
// Portable FNV path id (hex16) — not used for on-disk folder names (v0.1.8 uses std::hash).
std::string pathIdHex(const std::string& filePath);

// Filename stem ("/a/b/Book.epub" → "Book").
std::string fileStem(const std::string& path);

// Folder base name under /.crosspoint, e.g. "epub_1234567890".
std::string cacheFolderName(const std::string& filePath);

// /.crosspoint/epub_<std::hash> (or xtc_/txt_) — same dir as Epub::getCachePath().
std::string bookDirForPath(const std::string& filePath);
// Package files live in the cache root (v0.1.8); same as bookDirForPath.
std::string packageDirForPath(const std::string& filePath);
// Rivulet IR: <cache>/rivulet/
std::string rivuletDirForPath(const std::string& filePath);

// idOrFolder: return value of openBook (cache folder name) or legacy book_<hex>.
std::string bookDir(const std::string& idOrFolder);
std::string packageDir(const std::string& idOrFolder);
std::string rivuletDir(const std::string& idOrFolder);

// Ensure cache + rivulet dirs exist. No layout rekey. Returns cache folder name.
std::string openBook(const std::string& bookPath, const std::string& title, const std::string& author);

// API stubs kept so call sites compile; no ledger / book_* writes.
bool ensureBook(const std::string& idHex, const std::string& bookPath, const std::string& title);
bool lookupIdByPath(const std::string& bookPath, std::string& outIdHex);
bool lookupPathById(const std::string& idHex, std::string& outPath);
void writeMeta(const std::string& idHex, const std::string& bookPath, const std::string& title,
               const std::string& author);

uint64_t hashIdentity(const char* title, const char* author, const char* fileStem);
std::string legacyTitleIdHex(const std::string& title, const std::string& author, const std::string& filePath);

}  // namespace CasperBook
