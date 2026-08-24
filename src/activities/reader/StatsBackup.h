#pragma once

#include <cstddef>

#include "StatsBackupLayout.h"

// Copies /.crosspoint/global_stats.bin to /.crosspoint-stats-backup/ using a dated
// or incrementing filename. Does not walk per-book cache folders.
bool backupGlobalStats(bool manual, char* outFileName = nullptr, size_t outFileNameLen = 0);

// Deletes oldest lifetime backup files beyond the keep count. Returns removed count.
int pruneBackups(int keep = 7);

// Rename this book's live stats*.bin into <cache>/.trash/ (never unlink).
bool stashDeletedBookStats(const char* bookPath);

// Rename <cache>/.trash/stats*.bin back to live. Recover Stats in the long-press menu.
bool restoreBookStats(const char* bookPath);

// True when <cache>/.trash/ holds stats for this book.
bool hasRestorableBookStats(const char* bookPath);
