#pragma once

#include <string>

// Clears derived reading cache (layout/thumbs/parse) for a book if the
// extension is recognised (EPUB, XTC, or TXT). Preserves resume progress,
// per-book stats, reader settings, and dictionary history.
void clearBookCache(const std::string& path);

// Clears a known book cache directory while preserving user state:
// progress.bin(+.bak), reader_settings.bin, stats*.bin, dictionary_history.txt.
// Used by Settings → Clear Cache and per-book clear actions.
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
