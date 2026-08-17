#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IrFormat.h"

namespace rivulet {

// Cursor into ChapterIr for a page start.
struct IrCursor {
  uint16_t blockIndex = 0;
  uint16_t runIndex = 0;   // absolute run index in chapter
  uint16_t byteInRun = 0;  // UTF-8 byte offset within run

  bool operator==(const IrCursor& o) const {
    return blockIndex == o.blockIndex && runIndex == o.runIndex && byteInRun == o.byteInRun;
  }
  bool operator!=(const IrCursor& o) const { return !(*this == o); }
};

// Thin page map: page i begins at cursors[i]. Built by full layout pass or progressive fill.
class PageMap {
 public:
  void clear();
  void setRenderKey(const RenderKey& k) { key_ = k; }
  [[nodiscard]] const RenderKey& renderKey() const { return key_; }

  void resetWithStart(const IrCursor& firstPageStart);
  void pushPageStart(const IrCursor& c);
  // Overwrite page start (e.g. re-layout produced a different end). Truncates any
  // later entries so they cannot point past a gap/overlap. Marks map incomplete.
  void setPageStart(int pageIndex, const IrCursor& c);
  // Drop starts from pageIndex onward (keep [0, pageIndex)).
  void truncateFrom(int pageIndex);
  // Chapter fully walked: total must match starts_.size() (page count = starts).
  void markComplete(const int totalPages) {
    complete_ = true;
    // Prefer live start count — never trust a larger/stale total than we have.
    const int n = static_cast<int>(starts_.size());
    if (totalPages > 0 && (n <= 0 || totalPages == n)) {
      knownTotal_ = totalPages;
    } else {
      knownTotal_ = n;
    }
  }
  // Drop the complete flag (e.g. live layout found more content after last start).
  void markIncomplete() {
    complete_ = false;
    knownTotal_ = 0;
  }

  [[nodiscard]] bool empty() const { return starts_.empty(); }
  [[nodiscard]] bool complete() const { return complete_; }
  [[nodiscard]] int knownPages() const { return static_cast<int>(starts_.size()); }
  // While complete, knownTotal is the chapter length; if starts grew past a stale
  // total (false complete), report the larger live count.
  [[nodiscard]] int knownTotal() const {
    const int n = static_cast<int>(starts_.size());
    if (complete_ && knownTotal_ > 0) return knownTotal_ > n ? knownTotal_ : n;
    return n;
  }
  [[nodiscard]] IrCursor pageStart(int pageIndex) const;
  [[nodiscard]] bool hasPage(int pageIndex) const {
    return pageIndex >= 0 && pageIndex < static_cast<int>(starts_.size());
  }

  bool saveToFile(const char* path) const;
  bool loadFromFile(const char* path);

 private:
  RenderKey key_{};
  std::vector<IrCursor> starts_;
  bool complete_ = false;
  int knownTotal_ = 0;
};

}  // namespace rivulet
