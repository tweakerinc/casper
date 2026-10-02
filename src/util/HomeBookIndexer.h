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
// Work is sliced so no single step() blocks the Home loop. The first version did
// a whole chapter per call — convert plus a full page-map walk — which took 8-15
// seconds with no yield. Home stopped sampling input for that entire time and
// the device looked frozen. Now a step is either one chapter convert or a short
// burst of page measurements, and the chapter stays resident between bursts.
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

  // Advance the index by one bounded slice of work.
  // Returns true if it did real work (caller should yield and re-check input).
  // Returns false when there is nothing to do, heap is too tight, or the book is
  // fully indexed — check finished() to tell those apart.
  bool step(GfxRenderer& renderer);

  // True while a chapter is loaded and being measured, i.e. there is background
  // work in flight that a UI hint may want to show.
  [[nodiscard]] bool working() const { return activeSpine_ >= 0; }

 private:
  bool ensureOpen();
  // Load the next spine that has no map yet. Returns false when none remain.
  bool beginNextChapter(GfxRenderer& renderer);
  // Measure up to kPagesPerStep more pages of the resident chapter.
  void measureBurst(GfxRenderer& renderer);
  void finishChapter(bool mapped);
  void mapPathFor(int spine, char* out, size_t outSize) const;

  // Pages measured per step. Each is a full layout pass over one screen of text
  // (tens of ms), so this trades indexing throughput for input latency.
  static constexpr int kPagesPerStep = 4;
  // A chapter that never completes must not be retried forever.
  static constexpr int kMaxBurstsPerChapter = 400;

  std::string bookPath_;
  std::string irDir_;
  std::unique_ptr<Epub> epub_;
  // Opaque so this header does not drag RivuletEngine into HomeActivity.
  struct Engine;
  std::unique_ptr<Engine> engine_;
  int nextSpine_ = 0;
  int indexed_ = 0;
  // Spine currently loaded and being measured; -1 when between chapters.
  int activeSpine_ = -1;
  int burstsThisChapter_ = 0;
  uint32_t chapterStartMs_ = 0;
  bool finished_ = false;
  bool openFailed_ = false;
};
