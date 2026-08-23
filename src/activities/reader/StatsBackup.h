#pragma once

#include <cstddef>

#include "StatsBackupLayout.h"

// Copies /.crosspoint/global_stats.bin to /.casper-stats-backup/ using a dated
// or incrementing filename, then copies each book's stats_vN.bin into
// books/<snapId>/ as a raw 75-byte file (one at a time, no DRAM dump).
// Returns true if the lifetime file was written. Per-book copies are best-effort.
bool backupGlobalStats(bool manual, char* outFileName = nullptr, size_t outFileNameLen = 0);

// Deletes oldest lifetime backup files beyond the keep count. Returns removed count.
int pruneBackups(int keep = 7);

// Copy this book's live stats file to trash/<folder>.bin before Delete Book Stats.
bool stashDeletedBookStats(const char* bookPath);

// Restore live stats from trash, else the newest snapshot that has this book.
bool restoreBookStats(const char* bookPath);

// True when trash or any snapshot has a file for this book. Menu can hide Restore.
bool hasRestorableBookStats(const char* bookPath);
