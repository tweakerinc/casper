#pragma once

#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"

class GfxRenderer;

// Shared book maintenance actions used by long-press menus (Recent Books, Home cover).
namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);

bool hasClearableBookCache(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
bool restoreBookStatsForBook(const std::string& fullPath);
bool resetReadingPace(const std::string& fullPath);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg);

// Calibre/OPF synopsis for EPUB paths; empty if missing or not an EPUB.
std::string loadBookDescription(const std::string& fullPath);

}  // namespace BookActions
