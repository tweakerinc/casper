#pragma once

#include <string>

namespace BookMoveUtils {

std::string buildReadFolderDestination(const std::string& srcPath);

// Migrate cache dir, bookmarks, clippings, recents after a book path change
// (move to /Read, web rename, USB rename, WebDAV MOVE). Prefer this over
// clearBookCache so stats + covers follow the file without manual surgery.
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

// Detect book type from extension, migrate cache + side state after rename.
// Safe no-op for non-book paths. keepInRecents defaults to true for renames.
bool migrateRenamedBook(const std::string& oldPath, const std::string& newPath, bool keepInRecents = true);

}  // namespace BookMoveUtils
