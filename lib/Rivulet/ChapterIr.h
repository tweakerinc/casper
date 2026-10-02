#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include "../Memory/FallibleVector.h"

#include "IrFormat.h"
#include "PagedRecords.h"
#include <memory>

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

static_assert(sizeof(Block) == 16 && sizeof(Run) == 8, "IR record layout");

// In-memory chapter IR (Tier B working set). One chapter at a time.
// Text is a malloc buffer — never std::string growth (that aborts under -fno-exceptions).
class ChapterIr {
 public:
  ChapterIr();
  ~ChapterIr();
  ChapterIr(const ChapterIr&) = delete;
  ChapterIr& operator=(const ChapterIr&) = delete;
  ChapterIr(ChapterIr&& o) noexcept;
  ChapterIr& operator=(ChapterIr&& o) noexcept;

  // Spill chapter text AND metadata to SD with a constant-size working window.
  // The base path is a derived cache; unique scratch files are never user data.
  bool beginPaged(const char* basePath);
  bool diskBacked() const { return disk_ != nullptr; }
  static constexpr size_t kMaxRunBytes = 4096;
  static constexpr size_t kMaxDiskText = 32U * 1024U * 1024U;
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

  [[nodiscard]] const PagedRecords<Block>& blocks() const { return blocks_; }
  [[nodiscard]] PagedRecords<Block>& blocksMutable() { estimateCached_=-1; return blocks_; }
  [[nodiscard]] const PagedRecords<Run>& runs() const { return runs_; }
  [[nodiscard]] const char* textData() const { return textData_ ? textData_ : ""; }
  [[nodiscard]] size_t textSize() const { return textLen_; }
  // Compatibility for call sites that used textBlob().size().
  [[nodiscard]] size_t textBlobSize() const { return textLen_; }
  [[nodiscard]] size_t blockCount() const { return blocks_.size(); }
  [[nodiscard]] bool empty() const { return blocks_.empty(); }
  [[nodiscard]] bool failed() const;
  void clearFailed() { failed_ = false; }
  void markFailed() { failed_ = true; }

  [[nodiscard]] const char* runText(const Run& r) const;
  [[nodiscard]] std::string runString(const Run& r) const;
  // Replace run text (append-only: old blob bytes are orphaned). Used after image
  // probe to store package-absolute hrefs so paint does not re-resolve baseDir.
  bool setRunText(size_t runIndex, const char* utf8, size_t len);
  bool setRunText(size_t runIndex, const std::string& s) { return setRunText(runIndex, s.data(), s.size()); }

  size_t serializedSize() const;
  bool writeRangeTo(HalFile& file,size_t offset,size_t bytes) const;
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
  mutable int estimateCached_=-1, estimateW_=0,estimateH_=0,estimateEm_=0;
  mutable float estimateLc_=0;
  mutable bool failed_ = false;
  struct Disk;
  std::unique_ptr<Disk> disk_;
  PagedRecords<Block> blocks_;
  PagedRecords<Run> runs_;
  char* textData_ = nullptr;
  size_t textLen_ = 0;
  size_t textCap_ = 0;

  bool writeTo(HalFile& f) const;
  bool appendText(const char* s, size_t n);
  bool setupDisk(const char* path);
  LoadResult mountPaged(const char* path, uint32_t blocks, uint32_t runs, uint32_t text);
};

}  // namespace rivulet
