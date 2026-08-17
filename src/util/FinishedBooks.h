#pragma once

#include <string>
#include <vector>

// Finished books live in a fixed SD folder "/read".
// Hidden from Library root listing; browse via Recents → "Show Read Books".
// Package + progress/stats: /.crosspoint/epub_<std::hash>/ (same as shipped v0.1.8).
namespace FinishedBooks {

// Canonical finished-books directory (no trailing slash).
constexpr char FOLDER[] = "/read";
// Brief experiment name — still treated as finished if present on SD.
constexpr char ALT_FOLDER[] = "/Finished Books";
// Sidecar in the book's cache after a move: original full path for undo.
constexpr char ORIGIN_SIDECAR[] = "origin_path.txt";

// True if path is under /read or /Finished Books.
bool isInFinishedFolder(const std::string& path);

// True if this directory name (optional trailing '/') is the finished-books folder.
// Used to hide the folder from Library (always, not only when "Show Hidden" is off).
bool isFinishedDirName(const std::string& nameWithOptionalSlash);

// Destination path for a finished EPUB (collision-safe name under FOLDER).
std::string buildDestination(const std::string& srcPath);

// Move EPUB + re-key cache. Writes origin sidecar for undo.
// Returns new EPUB path on success, empty string on failure.
std::string moveToFinished(const std::string& srcPath);

// Restore EPUB + cache to origin path from sidecar (Mark Unfinished).
// Returns restored path on success, empty on failure / not in finished folder.
std::string restoreFromFinished(const std::string& currentPath);

// List book files under finished folders (for Recents → Show Read Books).
// Each entry: full path, display title (filename stem), empty author.
struct FinishedBookEntry {
  std::string path;
  std::string title;
};
void listFinishedBooks(std::vector<FinishedBookEntry>& out);

}  // namespace FinishedBooks
