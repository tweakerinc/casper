#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Epub;
class GfxRenderer;

// Builds the page maps for a whole book, one chapter per call, while the reader
// is closed.
//
// Why on Home and not in the reader: indexing needs the chapter IR resident, and
// the reader already has a chapter resident. Doing it there means evicting the
// page under the user and reloading it afterwards — that made page turns stop
// painting for seconds at a time. On Home nothing has to be given up, and free
// heap is at its highest (~110 KB vs ~70 KB mid-read). It also keeps the work out
// of reading-pace stats, since no page turns are happening.
//
// Each step() call does at most one chapter and returns quickly enough to keep
// the Home loop responsive; the caller decides how often to step and stops the
// moment the user touches anything.
class HomeBookIndexer {
 public:
  HomeBookIndexer();
  ~HomeBookIndexer();

  HomeBookIndexer(const HomeBookIndexer&) = delete;
  HomeBookIndexer& operator=(const HomeBookIndexer&) = delete;

  // Point the indexer at a book. Cheap: no SD work until the first step().
  // Re-targeting a different book restarts progress.
  void begin(const std::string& bookPath);

  // Release the Epub + engine. Call before leaving Home or opening a book so the
  // reader gets the heap back.
  void reset();

  [[nodiscard]] bool active() const { return !bookPath_.empty() && !finished_; }
  [[nodiscard]] bool finished() const { return finished_; }
  [[nodiscard]] const std::string& bookPath() const { return bookPath_; }
  // Chapters mapped this session (for logging / a future progress hint).
  [[nodiscard]] int indexedCount() const { return indexed_; }

  // Index the next chapter that has no page map yet.
  // Returns true if it did real work (caller should yield and re-check input).
  // Returns false when there is nothing to do, heap is too tight, or the book is
  // fully indexed — check finished() to tell those apart.
  bool step(GfxRenderer& renderer);

 private:
  bool ensureOpen();

  std::string bookPath_;
  std::string irDir_;
  std::unique_ptr<Epub> epub_;
  // Opaque so this header does not drag RivuletEngine into HomeActivity.
  struct Engine;
  std::unique_ptr<Engine> engine_;
  int nextSpine_ = 0;
  int indexed_ = 0;
  bool finished_ = false;
  bool openFailed_ = false;
};
