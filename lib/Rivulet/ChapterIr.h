#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "IrFormat.h"

class HalFile;

namespace rivulet {

// One styled text span. Text lives in ChapterIr text blob as a contiguous UTF-8 region.
struct Run {
  uint32_t textOff = 0;  // offset into chapter text blob
  uint16_t textLen = 0;  // bytes
  RunStyle style = RunStyle::Regular;
  SizeStep sizeStep = SizeStep::Body;
};

// One block-level unit (paragraph, heading, …).
struct Block {
  BlockKind kind = BlockKind::Paragraph;
  Align align = Align::Left;
  uint16_t flags = 0;
  uint8_t indentEmQ4 = 0;       // first-line indent in 1/16 em (0 = default policy)
  int8_t marginTopEmQ4 = 0;     // before block, 1/16 em (signed for future)
  int8_t marginBottomEmQ4 = 4;  // after block (~0.25em default)
  uint16_t runBegin = 0;        // index into ChapterIr::runs_
  uint16_t runCount = 0;
  // Image blocks only: display size after fit-to-viewport (0 = unknown / placeholder).
  uint16_t imageW = 0;
  uint16_t imageH = 0;
};

// In-memory chapter IR (Tier B working set). One chapter at a time.
// Text is a malloc buffer — never std::string growth (that aborts under -fno-exceptions).
class ChapterIr {
 public:
  ChapterIr() = default;
  ~ChapterIr() { freeText(); }
  ChapterIr(const ChapterIr&) = delete;
  ChapterIr& operator=(const ChapterIr&) = delete;
  ChapterIr(ChapterIr&& o) noexcept { *this = std::move(o); }
  ChapterIr& operator=(ChapterIr&& o) noexcept {
    if (this == &o) return *this;
    freeText();
    openBlock_ = o.openBlock_;
    failed_ = o.failed_;
    blocks_ = std::move(o.blocks_);
    runs_ = std::move(o.runs_);
    textData_ = o.textData_;
    textLen_ = o.textLen_;
    textCap_ = o.textCap_;
    o.textData_ = nullptr;
    o.textLen_ = 0;
    o.textCap_ = 0;
    o.openBlock_ = false;
    o.failed_ = false;
    return *this;
  }

  void clear();

  void reserveForConvert(size_t htmlLen);
  void beginBlock(BlockKind kind, Align align, uint16_t flags);
  void endBlock();
  // Returns false if OOM / cap hit (chapter may be partial; failed() is true).
  bool appendRun(RunStyle style, SizeStep step, const char* utf8, size_t len);
  bool appendRun(RunStyle style, SizeStep step, const std::string& s) {
    return appendRun(style, step, s.data(), s.size());
  }
  void setCurrentIndentEmQ4(uint8_t v);
  void setCurrentMarginsEmQ4(int8_t top, int8_t bottom);
  void markDropCapOnCurrent();

  [[nodiscard]] const std::vector<Block>& blocks() const { return blocks_; }
  [[nodiscard]] std::vector<Block>& blocksMutable() { return blocks_; }
  [[nodiscard]] const std::vector<Run>& runs() const { return runs_; }
  [[nodiscard]] const char* textData() const { return textData_ ? textData_ : ""; }
  [[nodiscard]] size_t textSize() const { return textLen_; }
  // Compatibility for call sites that used textBlob().size().
  [[nodiscard]] size_t textBlobSize() const { return textLen_; }
  [[nodiscard]] size_t blockCount() const { return blocks_.size(); }
  [[nodiscard]] bool empty() const { return blocks_.empty(); }
  [[nodiscard]] bool failed() const { return failed_; }
  void clearFailed() { failed_ = false; }
  void markFailed() { failed_ = true; }

  [[nodiscard]] const char* runText(const Run& r) const;
  [[nodiscard]] std::string runString(const Run& r) const;
  // Replace run text (append-only: old blob bytes are orphaned). Used after image
  // probe to store package-absolute hrefs so paint does not re-resolve baseDir.
  bool setRunText(size_t runIndex, const char* utf8, size_t len);
  bool setRunText(size_t runIndex, const std::string& s) { return setRunText(runIndex, s.data(), s.size()); }

  bool saveToFile(const char* path) const;
  bool loadFromFile(const char* path);
  // Distinguish OOM from a bad header so callers can keep a just-read cache.
  enum class LoadResult : uint8_t { Ok, Corrupt, Oom, StaleVersion };
  LoadResult loadFromFileEx(const char* path);

  [[nodiscard]] int estimatePageCount(int viewportW, int viewportH, int bodyEmPx, float lineCompression) const;

 private:
  void freeText();
  bool ensureTextCapacity(size_t needExtra);
  bool ensureRunsCapacity(size_t needExtra);

  bool openBlock_ = false;
  bool failed_ = false;
  std::vector<Block> blocks_;
  std::vector<Run> runs_;
  char* textData_ = nullptr;
  size_t textLen_ = 0;
  size_t textCap_ = 0;

  bool writeTo(HalFile& f) const;
  LoadResult readFrom(HalFile& f);
};

}  // namespace rivulet
