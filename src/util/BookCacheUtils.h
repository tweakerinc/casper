#pragma once

#include <string>

// Clears derived reading cache (layout/thumbs/parse/Rivulet IR) for a book if the
// extension is recognised (EPUB, XTC, or TXT). Preserves resume progress,
// per-book stats, reader settings, and dictionary history.
void clearBookCache(const std::string& path);

// Recursively delete a directory after closing every handle. FAT cannot rmdir
// an open folder; Storage.removeDir() can delete book.bin and leave rivulet/
// (so the next open reuses a finished .rvpm and looks like "cache was not
// deleted"). Safe no-op if path is empty or missing.
bool wipeCacheDirectory(const std::string& path);

// Clears a known book cache directory while preserving user state:
// progress.bin(+.bak), reader_settings.bin, stats*.bin, dictionary_history.txt.
// Used by Settings → Clear Cache and per-book clear actions.
//
// Accepts either:
//   - legacy package dir (epub_*/txt_*/xtc_*) — wipe contents, restore preserve list
//   - unified book_<id> ownership dir — wipe package/ + rivulet/, keep progress/stats
//   - package/ subdir of book_<id> — also clears sibling rivulet/
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry under /.crosspoint
// (or legacy /.casper): book_*, epub_*, txt_*, xtc_*.
bool isBookCacheDirectoryName(const char* name);
