#pragma once

#include <cstdint>
#include <string>

#include "activities/reader/BookReadingStats.h"

// Casper-owned stats facade.
//
// Contract (v0.1.8 layout — unchanged on upgrade):
//   - Book folder: /.crosspoint/epub_<std::hash(path)>/ (same as package cache)
//   - Progress for Home lives on RecentBook (recent.json) — zero stats files on paint
//   - load/save go only through this module for new product code
//
// BookReadingStats remains the in-memory/record layout (binary compatible with
// stats_v6 on disk).

namespace CasperStats {

// Primary ownership dir for a book file path (empty if path empty).
std::string bookDir(const std::string& bookFilePath);

// Load per-book stats from the v0.1.8 cache folder only.
BookReadingStats loadBook(const std::string& bookFilePath);

// Save per-book stats under bookDir + push progress into recent.json.
void saveBook(const std::string& bookFilePath, const BookReadingStats& stats);

// Home/list: prefer progress embedded in RecentBooksStore; else loadBook once.
float homeProgressPercent(const std::string& bookFilePath);

// Update Home progress without a full stats save (reader page leave / mark done).
void setHomeProgress(const std::string& bookFilePath, float percent0to100);

// Delete stats under the Casper book dir only.
bool removeBook(const std::string& bookFilePath);

}  // namespace CasperStats
