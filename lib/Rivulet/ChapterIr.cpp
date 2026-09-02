#include "ChapterIr.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <Esp.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace rivulet {
namespace {

constexpr size_t kMaxTextBlob = 192 * 1024;
constexpr size_t kMaxBlocks = 4096;
constexpr size_t kMaxRuns = 32768;

bool canAlloc(const size_t bytes) {
  if (bytes == 0) return true;
  if (ESP.getMaxAllocHeap() < bytes + 512) return false;
  void* p = std::malloc(bytes);
  if (!p) return false;
  std::free(p);
  return true;
}

}  // namespace

void ChapterIr::freeText() {
  if (textData_) {
    std::free(textData_);
    textData_ = nullptr;
  }
  textLen_ = 0;
  textCap_ = 0;
}

void ChapterIr::clear() {
  openBlock_ = false;
  failed_ = false;
  blocks_.clear();
  runs_.clear();
  freeText();
}

bool ChapterIr::ensureTextCapacity(const size_t needExtra) {
  const size_t need = textLen_ + needExtra;
  if (need <= textCap_) return true;
  if (need > kMaxTextBlob) return false;
  // Grow by ~2x (min +4KB) so convert does few reallocs. Prefer realloc (may grow
  // in place) over malloc+copy+free — that path fragmented maxAlloc mid-chapter
  // and produced partial IR ("last page" stopped at the totem line).
  size_t newCap = textCap_ ? textCap_ : 4096;
  while (newCap < need) {
    if (newCap > kMaxTextBlob / 2) {
      newCap = need;
      break;
    }
    const size_t grown = newCap + std::max<size_t>(4096, newCap);
    newCap = std::min(kMaxTextBlob, grown);
  }
  if (newCap < need) return false;
  // Prefer realloc (may grow in place). Avoid malloc+copy+free thrash that
  // fragmented maxAlloc mid-convert and truncated chapters.
  char* p = static_cast<char*>(std::realloc(textData_, newCap));
  if (!p) {
    LOG_ERR("RVIR", "OOM text realloc need=%u cap=%u free=%u maxA=%u", static_cast<unsigned>(need),
            static_cast<unsigned>(newCap), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  textData_ = p;
  textCap_ = newCap;
  return true;
}

bool ChapterIr::ensureRunsCapacity(const size_t needExtra) {
  const size_t need = runs_.size() + needExtra;
  if (need <= runs_.capacity()) return true;
  if (need > kMaxRuns) return false;
  size_t newCap = runs_.capacity() ? runs_.capacity() : 64;
  while (newCap < need) {
    newCap = std::min(kMaxRuns, newCap < 256 ? newCap + 64 : newCap * 2);
  }
  if (newCap < need) return false;
  // vector::reserve allocates a full new buffer — probe full size.
  if (!canAlloc(newCap * sizeof(Run) + 64)) return false;
  runs_.reserve(newCap);
  return runs_.capacity() >= need;
}

void ChapterIr::reserveForConvert(const size_t htmlLen) {
  // One-shot pre-size under the caller's FB loan so mid-convert does not thrash
  // realloc and fragment maxAlloc (partial IR → false chapter end).
  // Prose XHTML is typically ~40–60% text; leave room for HTML still in RAM + vectors.
  const size_t maxA = ESP.getMaxAllocHeap();
  if (maxA < 12 * 1024) return;

  size_t textGuess = htmlLen > 0 ? (htmlLen * 3 / 5) + 1024 : 4096;
  if (textGuess > kMaxTextBlob) textGuess = kMaxTextBlob;
  // Cap text pre-size to ~half of max contiguous so vectors still fit.
  const size_t textCap = std::min(textGuess, maxA / 2);
  if (textCap >= 2048 && (!textData_ || textCap_ < textCap)) {
    char* p = static_cast<char*>(std::realloc(textData_, textCap));
    if (p) {
      textData_ = p;
      textCap_ = textCap;
    }
  }

  const size_t blockGuess = std::min(kMaxBlocks, std::max<size_t>(48, htmlLen / 180 + 16));
  const size_t runGuess = std::min(kMaxRuns, std::max<size_t>(96, htmlLen / 90 + 32));
  if (blocks_.capacity() < blockGuess && canAlloc(blockGuess * sizeof(Block) + 64)) {
    blocks_.reserve(blockGuess);
  }
  if (runs_.capacity() < runGuess && canAlloc(runGuess * sizeof(Run) + 64)) {
    runs_.reserve(runGuess);
  }
}

void ChapterIr::beginBlock(const BlockKind kind, const Align align, const uint16_t flags) {
  if (failed_) return;
  if (openBlock_) endBlock();
  if (blocks_.size() >= kMaxBlocks) {
    LOG_ERR("RVIR", "block cap %u", static_cast<unsigned>(kMaxBlocks));
    failed_ = true;
    return;
  }
  if (blocks_.size() == blocks_.capacity()) {
    size_t nc = blocks_.capacity() ? blocks_.capacity() + 16 : 32;
    if (nc > kMaxBlocks) nc = kMaxBlocks;
    if (nc <= blocks_.capacity() || !canAlloc(nc * sizeof(Block) + 64)) {
      LOG_ERR("RVIR", "OOM blocks free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      failed_ = true;
      return;
    }
    blocks_.reserve(nc);
  }
  Block b;
  b.kind = kind;
  b.align = align;
  b.flags = flags;
  b.runBegin = static_cast<uint16_t>(std::min<size_t>(runs_.size(), 65535));
  b.runCount = 0;
  if (kind >= BlockKind::Heading1 && kind <= BlockKind::Heading6) {
    b.flags |= kBlockNoIndent;
    b.align = Align::Center;
    b.marginTopEmQ4 = 4;
    b.marginBottomEmQ4 = 6;
  } else if (kind == BlockKind::Paragraph) {
    b.indentEmQ4 = 16;
    b.marginTopEmQ4 = 0;
    b.marginBottomEmQ4 = 0;
  } else if (kind == BlockKind::HorizontalRule || kind == BlockKind::Spacer) {
    b.flags |= kBlockNoIndent;
    b.marginTopEmQ4 = 8;
    b.marginBottomEmQ4 = 8;
  } else if (kind == BlockKind::Image) {
    b.flags |= kBlockNoIndent;
    b.align = Align::Center;
    b.marginTopEmQ4 = 4;
    b.marginBottomEmQ4 = 4;
    b.indentEmQ4 = 0;
  }
  blocks_.push_back(b);
  openBlock_ = true;
}

void ChapterIr::endBlock() {
  if (!openBlock_ || blocks_.empty()) {
    openBlock_ = false;
    return;
  }
  Block& b = blocks_.back();
  const size_t end = runs_.size();
  b.runCount = static_cast<uint16_t>(end - b.runBegin);
  if (b.runCount == 0 &&
      (b.kind == BlockKind::Paragraph ||
       (b.kind >= BlockKind::Heading1 && b.kind <= BlockKind::Heading6))) {
    blocks_.pop_back();
  }
  openBlock_ = false;
}

bool ChapterIr::appendRun(const RunStyle style, const SizeStep step, const char* utf8, const size_t len) {
  if (failed_ || !openBlock_ || !utf8 || len == 0) return !failed_;
  if (runs_.size() >= kMaxRuns) {
    failed_ = true;
    return false;
  }
  if (textLen_ + len > kMaxTextBlob) {
    LOG_ERR("RVIR", "text blob cap %u", static_cast<unsigned>(kMaxTextBlob));
    failed_ = true;
    return false;
  }
  // Coalesce adjacent identical style runs.
  if (!runs_.empty() && blocks_.back().runCount > 0) {
    Run& last = runs_.back();
    if (last.style == style && last.sizeStep == step && last.textOff + last.textLen == textLen_) {
      if (!ensureTextCapacity(len)) {
        LOG_ERR("RVIR", "OOM coalesce free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        failed_ = true;
        return false;
      }
      std::memcpy(textData_ + textLen_, utf8, len);
      textLen_ += len;
      last.textLen = static_cast<uint16_t>(std::min<size_t>(65535, last.textLen + len));
      return true;
    }
  }
  if (!ensureRunsCapacity(1) || !ensureTextCapacity(len)) {
    LOG_ERR("RVIR", "OOM appendRun free=%u maxAlloc=%u need=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(len));
    failed_ = true;
    return false;
  }
  Run r;
  r.textOff = static_cast<uint32_t>(textLen_);
  r.textLen = static_cast<uint16_t>(std::min<size_t>(len, 65535));
  r.style = style;
  r.sizeStep = step;
  std::memcpy(textData_ + textLen_, utf8, r.textLen);
  textLen_ += r.textLen;
  runs_.push_back(r);
  blocks_.back().runCount++;
  return true;
}

void ChapterIr::setCurrentIndentEmQ4(const uint8_t v) {
  if (openBlock_ && !blocks_.empty()) blocks_.back().indentEmQ4 = v;
}

void ChapterIr::setCurrentMarginsEmQ4(const int8_t top, const int8_t bottom) {
  if (openBlock_ && !blocks_.empty()) {
    blocks_.back().marginTopEmQ4 = top;
    blocks_.back().marginBottomEmQ4 = bottom;
  }
}

void ChapterIr::markDropCapOnCurrent() {
  if (openBlock_ && !blocks_.empty()) {
    blocks_.back().flags = static_cast<uint16_t>(blocks_.back().flags | kBlockDropCap | kBlockNoIndent);
  }
}

const char* ChapterIr::runText(const Run& r) const {
  if (!textData_ || r.textOff + r.textLen > textLen_) return "";
  return textData_ + r.textOff;
}

std::string ChapterIr::runString(const Run& r) const {
  if (!textData_ || r.textOff + r.textLen > textLen_) return {};
  return std::string(textData_ + r.textOff, r.textLen);
}

bool ChapterIr::setRunText(const size_t runIndex, const char* utf8, const size_t len) {
  if (failed_ || !utf8 || len == 0 || runIndex >= runs_.size()) return false;
  if (textLen_ + len > kMaxTextBlob) {
    LOG_ERR("RVIR", "setRunText blob cap");
    return false;
  }
  if (!ensureTextCapacity(len)) {
    LOG_ERR("RVIR", "setRunText OOM free=%u maxA=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  Run& r = runs_[runIndex];
  // Same length: overwrite in place (no blob growth).
  if (r.textLen == len && r.textOff + len <= textLen_ && textData_) {
    std::memcpy(textData_ + r.textOff, utf8, len);
    return true;
  }
  r.textOff = static_cast<uint32_t>(textLen_);
  r.textLen = static_cast<uint16_t>(std::min<size_t>(len, 65535));
  std::memcpy(textData_ + textLen_, utf8, r.textLen);
  textLen_ += r.textLen;
  return true;
}

bool ChapterIr::writeTo(HalFile& f) const {
  if (!serialization::tryWritePod(f, kIrMagic)) return false;
  if (!serialization::tryWritePod(f, kIrFormatVersion)) return false;
  const uint32_t nBlocks = static_cast<uint32_t>(blocks_.size());
  const uint32_t nRuns = static_cast<uint32_t>(runs_.size());
  const uint32_t nText = static_cast<uint32_t>(textLen_);
  if (!serialization::tryWritePod(f, nBlocks)) return false;
  if (!serialization::tryWritePod(f, nRuns)) return false;
  if (!serialization::tryWritePod(f, nText)) return false;
  for (const Block& b : blocks_) {
    const uint8_t kind = static_cast<uint8_t>(b.kind);
    const uint8_t align = static_cast<uint8_t>(b.align);
    if (!serialization::tryWritePod(f, kind)) return false;
    if (!serialization::tryWritePod(f, align)) return false;
    if (!serialization::tryWritePod(f, b.flags)) return false;
    if (!serialization::tryWritePod(f, b.indentEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.marginTopEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.marginBottomEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.runBegin)) return false;
    if (!serialization::tryWritePod(f, b.runCount)) return false;
    if (!serialization::tryWritePod(f, b.imageW)) return false;
    if (!serialization::tryWritePod(f, b.imageH)) return false;
  }
  for (const Run& r : runs_) {
    if (!serialization::tryWritePod(f, r.textOff)) return false;
    if (!serialization::tryWritePod(f, r.textLen)) return false;
    const uint8_t st = static_cast<uint8_t>(r.style);
    const int8_t step = static_cast<int8_t>(r.sizeStep);
    if (!serialization::tryWritePod(f, st)) return false;
    if (!serialization::tryWritePod(f, step)) return false;
  }
  if (nText > 0 && textData_) {
    if (f.write(reinterpret_cast<const uint8_t*>(textData_), nText) != nText) return false;
  }
  return true;
}

bool ChapterIr::readFrom(HalFile& f) {
  clear();
  char magic[4] = {};
  if (!serialization::tryReadPod(f, magic)) return false;
  if (std::memcmp(magic, kIrMagic, 4) != 0) {
    LOG_ERR("RVIR", "bad magic");
    return false;
  }
  uint16_t ver = 0;
  if (!serialization::tryReadPod(f, ver) || ver < kIrFormatVersionMin || ver > kIrFormatVersionMax) {
    LOG_ERR("RVIR", "bad version %u", ver);
    return false;
  }
  uint32_t nBlocks = 0, nRuns = 0, nText = 0;
  if (!serialization::tryReadPod(f, nBlocks)) return false;
  if (!serialization::tryReadPod(f, nRuns)) return false;
  if (!serialization::tryReadPod(f, nText)) return false;
  if (nBlocks > kMaxBlocks || nRuns > kMaxRuns || nText > kMaxTextBlob) {
    LOG_ERR("RVIR", "corrupt counts b=%u r=%u t=%u", nBlocks, nRuns, nText);
    return false;
  }
  // resize can abort under -fno-exceptions if huge — cap checked above; probe first.
  if (nBlocks > 0 && !canAlloc(nBlocks * sizeof(Block) + 64)) return false;
  if (nRuns > 0 && !canAlloc(nRuns * sizeof(Run) + 64)) return false;
  if (nText > 0 && !canAlloc(nText + 64)) return false;
  blocks_.resize(nBlocks);
  runs_.resize(nRuns);
  for (uint32_t i = 0; i < nBlocks; ++i) {
    uint8_t kind = 0, align = 0;
    if (!serialization::tryReadPod(f, kind)) return false;
    if (!serialization::tryReadPod(f, align)) return false;
    Block& b = blocks_[i];
    b.kind = static_cast<BlockKind>(kind);
    b.align = static_cast<Align>(align);
    if (!serialization::tryReadPod(f, b.flags)) return false;
    if (!serialization::tryReadPod(f, b.indentEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.marginTopEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.marginBottomEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.runBegin)) return false;
    if (!serialization::tryReadPod(f, b.runCount)) return false;
    if (!serialization::tryReadPod(f, b.imageW)) return false;
    if (!serialization::tryReadPod(f, b.imageH)) return false;
  }
  for (uint32_t i = 0; i < nRuns; ++i) {
    Run& r = runs_[i];
    if (!serialization::tryReadPod(f, r.textOff)) return false;
    if (!serialization::tryReadPod(f, r.textLen)) return false;
    uint8_t st = 0;
    int8_t step = 2;
    if (!serialization::tryReadPod(f, st)) return false;
    if (!serialization::tryReadPod(f, step)) return false;
    r.style = static_cast<RunStyle>(st);
    r.sizeStep = static_cast<SizeStep>(step);
  }
  if (nText > 0) {
    textData_ = static_cast<char*>(std::malloc(nText));
    if (!textData_) return false;
    textCap_ = nText;
    textLen_ = nText;
    if (f.read(reinterpret_cast<uint8_t*>(textData_), nText) != static_cast<int>(nText)) return false;
  }
  return true;
}

bool ChapterIr::saveToFile(const char* path) const {
  if (!path || !*path) return false;
  HalFile f;
  if (!Storage.openFileForWrite("RVIR", path, f)) {
    LOG_ERR("RVIR", "save open failed %s", path);
    return false;
  }
  const bool ok = writeTo(f);
  f.close();
  if (!ok) Storage.remove(path);
  return ok;
}

bool ChapterIr::loadFromFile(const char* path) {
  if (!path || !*path) return false;
  HalFile f;
  if (!Storage.openFileForRead("RVIR", path, f)) return false;
  const bool ok = readFrom(f);
  f.close();
  if (!ok) clear();
  return ok;
}

int ChapterIr::estimatePageCount(const int viewportW, const int viewportH, const int bodyEmPx,
                                 const float lineCompression) const {
  if (viewportW < 16 || viewportH < 16 || bodyEmPx < 4) return 1;
  if (textLen_ == 0 && blocks_.empty()) return 1;
  const float lc = lineCompression > 0.1f ? lineCompression : 1.0f;
  const int lineHpx = std::max(bodyEmPx + 2, static_cast<int>(bodyEmPx * 1.2f * lc + 0.5f));
  const int linesPerPage = std::max(1, viewportH / lineHpx);
  const int charsPerLine = std::max(12, (viewportW * 10) / std::max(1, bodyEmPx * 5));
  const size_t chars = textLen_;
  const int paraBreaks = static_cast<int>(blocks_.size());
  const int contentLines =
      static_cast<int>((chars + static_cast<size_t>(charsPerLine) - 1) / static_cast<size_t>(charsPerLine)) +
      paraBreaks / 2;
  const int pages = std::max(1, (contentLines + linesPerPage - 1) / linesPerPage);
  if (pages == 1 && (chars > 400 || blocks_.size() > 3)) {
    return 2;
  }
  return pages;
}

}  // namespace rivulet
