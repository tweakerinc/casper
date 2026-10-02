#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  // CrossPoint Home progress: 0–10000 = 0.00–100.00%, 0xFFFF = unknown.
  // Written on reader leave so Home never opens per-book stats files.
  uint16_t progressPercentMilli = 0xFFFF;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 private:
  std::vector<RecentBook> recentBooks;
  // False until the first successful load attempt (including "no file yet").
  // Prevents addBook from writing a 1-entry list over a full recent.json when
  // boot deferred loadFromFile (Quick Resume) or load never ran.
  bool loadedOnce_ = false;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

  // Load from SD if not yet loaded. Safe to call before any mutate/save.
  void ensureLoaded();
  // Pull any disk entries missing from memory (path-keyed) onto the tail.
  void mergeMissingFromDisk();
  static bool parseBooksArray(JsonVariantConst doc, std::vector<RecentBook>& out);

 public:
  // Full history for Recents Menu (full-screen list). Home under-panel still
  // shows only 4 (X3) / 5 (X4) via theme metrics — not this cap.
  static constexpr int MAX_RECENT_BOOKS = 20;

  static const char* getFilePath() { return "/.crosspoint/recent.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add a book to the recent list (moves to front if already exists).
  // Always loads disk first so a deferred boot load cannot wipe history.
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  RecentBook getDataFromBook(std::string path) const;

  // Update Home progress for a path already on the list. Returns true if changed.
  bool setProgressMilli(const std::string& path, uint16_t progressPercentMilli);

  // Needed by CrossPointStats (ensureLoaded is private otherwise).
  void ensureLoadedPublic() { ensureLoaded(); }
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
