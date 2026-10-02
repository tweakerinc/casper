#include "ChapterIr.h"
#include "PagedText.h"

#include <Esp.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <new>

namespace rivulet {
namespace {

constexpr size_t kMaxTextBlob = 192 * 1024;
constexpr size_t kMaxBlocks = 65534;
constexpr size_t kMaxRuns = 65534;

bool canAlloc(const size_t bytes) {
  if (bytes == 0) return true;
  if (ESP.getMaxAllocHeap() < bytes + 512) return false;
  void* p = std::malloc(bytes);
  if (!p) return false;
  std::free(p);
  return true;
}

}  // namespace

struct ChapterIr::Disk {
  PagedText text;
  char prefix[240]{};
  mutable char run[kMaxRunBytes + 1]{};
  mutable uint32_t cachedOffset = UINT32_MAX;
  mutable uint16_t cachedLength = 0;
};

ChapterIr::ChapterIr() = default;
ChapterIr::~ChapterIr() { clear(); }
ChapterIr::ChapterIr(ChapterIr&& o) noexcept { *this = std::move(o); }
ChapterIr& ChapterIr::operator=(ChapterIr&& o) noexcept {
  if (this == &o) return *this;
  clear();
  openBlock_ = o.openBlock_; failed_ = o.failed_;
  blocks_ = std::move(o.blocks_); runs_ = std::move(o.runs_);
  disk_ = std::move(o.disk_);
  textData_ = o.textData_; textLen_ = o.textLen_; textCap_ = o.textCap_;
  o.textData_ = nullptr; o.textLen_ = o.textCap_ = 0;
  o.openBlock_ = o.failed_ = false;
  return *this;
}
bool ChapterIr::failed() const {
  return failed_ || blocks_.failed() || runs_.failed() || (disk_ && disk_->text.failed());
}
bool ChapterIr::setupDisk(const char* path) {
  if (!path || !*path) return false;
  if (!casper_memory::allowAllocation()) return false;
  disk_.reset(new (std::nothrow) Disk());
  if (!disk_) return false;
  static uint32_t serial = 0;  // callers hold the reader/render exclusion lock
  const int n = std::snprintf(disk_->prefix, sizeof(disk_->prefix), "%s.w%lu", path,
                              static_cast<unsigned long>(++serial));
  if (n < 0 || static_cast<size_t>(n) >= sizeof(disk_->prefix)) { disk_.reset(); return false; }
  return true;
}
bool ChapterIr::beginPaged(const char* path) {
  clear();
  if (!setupDisk(path)) return false;
  char name[256];
  std::snprintf(name, sizeof(name), "%s.b", disk_->prefix);
  if (!blocks_.create(name)) { clear(); return false; }
  std::snprintf(name, sizeof(name), "%s.r", disk_->prefix);
  if (!runs_.create(name)) { clear(); return false; }
  std::snprintf(name, sizeof(name), "%s.t", disk_->prefix);
  if (!disk_->text.create(name)) { clear(); return false; }
  return true;
}
bool ChapterIr::appendText(const char* s, size_t n) {
  if (disk_) return disk_->text.append(s, n);
  if (!ensureTextCapacity(n)) return false;
  if (n) std::memcpy(textData_ + textLen_, s, n);
  return true;
}

void ChapterIr::freeText() {
  if (textData_) {
    std::free(textData_);
    textData_ = nullptr;
  }
  textLen_ = 0;
  textCap_ = 0;
}

void ChapterIr::clear() {
  estimateCached_=-1;
  openBlock_ = false;
  failed_ = false;
  blocks_.release();
  runs_.release();
  freeText();
  disk_.reset();
}

bool ChapterIr::ensureTextCapacity(const size_t needExtra) {
  if (needExtra > kMaxTextBlob || textLen_ > kMaxTextBlob - needExtra) return false;
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
  return runs_.reserve(newCap);
}

void ChapterIr::reserveForConvert(const size_t htmlLen) {
  if (disk_) return;  // disk windows are fixed; no chapter-sized allocation
  // One-shot pre-size under the caller's FB loan so mid-convert does not thrash
  // realloc and fragment maxAlloc (partial IR → false chapter end).
  // Prose XHTML is typically ~40–60% text; leave room for HTML still in RAM + vectors.
  const size_t maxA = ESP.getMaxAllocHeap();
  if (maxA < 12 * 1024) return;

  size_t textGuess = htmlLen > 0 ? (htmlLen * 3 / 5) + 1024 : 4096;
  if (textGuess > kMaxTextBlob) textGuess = kMaxTextBlob;
  // Cap text pre-size to ~half of max contiguous so the runs/blocks vectors still
  // fit afterwards.
  //
  // Taking the full guess here was tried and measured worse: the same chapter went
  // from `partial=0 text=23006 blocks=158` to `partial=1 text=19826 blocks=120`.
  // Grabbing the whole estimate up front leaves too little contiguous heap for the
  // vector growth that follows, so the convert OOMs later instead of earlier. (That
  // attempt was chasing a misdiagnosis anyway — the real fault was PageLayouter
  // reporting failure for measure-only pages, fixed separately.)
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
    if (!blocks_.reserve(nc)) { failed_ = true; return; }
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
      (b.kind == BlockKind::Paragraph || (b.kind >= BlockKind::Heading1 && b.kind <= BlockKind::Heading6))) {
    blocks_.pop_back();
  }
  openBlock_ = false;
}

bool ChapterIr::appendRun(const RunStyle style, const SizeStep step, const char* utf8, const size_t len) {
  estimateCached_=-1;
  if (failed() || !openBlock_ || !utf8 || len == 0) return !failed();
  const size_t limit = disk_ ? kMaxDiskText : kMaxTextBlob;
  if (len > limit || textLen_ > limit - len) { failed_ = true; return false; }
  size_t off = 0;
  while (off < len) {
    bool join = !runs_.empty() && blocks_.back().runCount;
    Run last{};
    if (join) {
      last = static_cast<const PagedRecords<Run>&>(runs_).back();
      join = last.style == style && last.sizeStep == step && last.textOff + last.textLen == textLen_ &&
             last.textLen < kMaxRunBytes;
    }
    // Split at a real word separator when a storage run would otherwise end in
    // a word. The suffix stays in the text store: only two tiny records change.
    if (join && last.textLen + len - off > kMaxRunBytes && last.textLen > 1024) {
      const char* bytes = runText(last);
      size_t cut = last.textLen;
      while (cut > last.textLen / 2 && bytes[cut-1] != ' ' && bytes[cut-1] != '\t') --cut;
      if (cut < last.textLen && cut > last.textLen / 2) {
        Run tail = last; tail.textOff += cut; tail.textLen -= cut;
        if (!ensureRunsCapacity(1)) { failed_ = true; return false; }
        runs_.back().textLen = static_cast<uint16_t>(cut);
        if (!runs_.push_back(tail)) { failed_ = true; return false; }
        ++blocks_.back().runCount; last = tail;
      }
    }
    size_t take = std::min<size_t>(len-off, join ? kMaxRunBytes-last.textLen : kMaxRunBytes);
    if (off+take < len) {
      while (take && (static_cast<unsigned char>(utf8[off+take]) & 0xc0) == 0x80) --take;
      // Prefer a word boundary for new long source spans as well.
      size_t cut = take;
      while (cut > take/2 && utf8[off+cut-1] != ' ' && utf8[off+cut-1] != '\t') --cut;
      if (cut > take/2) take = cut;
    }
    if (!take) { join = false; take = std::min<size_t>(len-off, kMaxRunBytes); }
    if (!join && !ensureRunsCapacity(1)) { failed_ = true; return false; }
    if (!appendText(utf8+off, take)) { failed_ = true; return false; }
    if (join) runs_.back().textLen = static_cast<uint16_t>(last.textLen + take);
    else {
      Run run; run.textOff = static_cast<uint32_t>(textLen_); run.textLen = static_cast<uint16_t>(take);
      run.style = style; run.sizeStep = step;
      if (!runs_.push_back(run)) { failed_ = true; return false; }
      ++blocks_.back().runCount;
    }
    textLen_ += take; off += take;
  }
  return !failed();
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
  if (r.textOff > textLen_ || r.textLen > textLen_ - r.textOff || r.textLen > kMaxRunBytes) {
    failed_ = true;
    if (disk_) { std::memset(disk_->run, 0, sizeof(disk_->run)); return disk_->run; }
    return "";
  }
  if (!disk_) return textData_ ? textData_ + r.textOff : "";
  if (disk_->cachedOffset != r.textOff || disk_->cachedLength != r.textLen) {
    if (!disk_->text.readRange(r.textOff, disk_->run, r.textLen)) {
      failed_ = true; std::memset(disk_->run, 0, sizeof(disk_->run));
    }
    disk_->run[r.textLen] = 0;
    disk_->cachedOffset = r.textOff; disk_->cachedLength = r.textLen;
  }
  return disk_->run;
}
std::string ChapterIr::runString(const Run& r) const {
  const char* text = runText(r);
  return failed() ? std::string() : std::string(text, r.textLen);
}
bool ChapterIr::setRunText(size_t index, const char* s, size_t len) {
  if (failed() || index >= runs_.size() || !s || len > kMaxRunBytes ||
      textLen_ > (disk_ ? kMaxDiskText : kMaxTextBlob) - len) return false;
  if (!appendText(s,len)) { failed_ = true; return false; }
  Run& run = runs_[index]; run.textOff = static_cast<uint32_t>(textLen_); run.textLen = static_cast<uint16_t>(len);
  textLen_ += len;
  if (disk_) disk_->cachedOffset = UINT32_MAX;
  return !failed();
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
  if (!blocks_.writeTo(f) || !runs_.writeTo(f)) return false;
  if (disk_) return !failed() && disk_->text.writeTo(f);
  if (nText > 0 && textData_) {
    if (f.write(reinterpret_cast<const uint8_t*>(textData_), nText) != nText) return false;
  }
  return true;
}

bool ChapterIr::saveToFile(const char* path) const {
  // A usable prefix is not a complete chapter and must never replace one.
  if (!path || !*path || failed()) return false;
  char temp[256];
  const int n = std::snprintf(temp, sizeof(temp), "%s.tmp", path);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(temp)) return false;
  HalFile f;
  if (!Storage.openFileForWrite("RVIR", temp, f)) return false;
  const bool ok = writeTo(f);
  f.close();
  if (!ok) { Storage.remove(temp); return false; }
  // Derived cache only: a loss between remove and rename causes a rebuild;
  // a short write never truncates the previously valid cache.
  if (Storage.exists(path) && !Storage.remove(path)) { Storage.remove(temp); return false; }
  if (!Storage.rename(temp, path)) { Storage.remove(temp); return false; }
  return true;
}

ChapterIr::LoadResult ChapterIr::mountPaged(const char* path, uint32_t nb, uint32_t nr, uint32_t nt) {
  if (!setupDisk(path)) return LoadResult::Oom;
  char name[256];
  std::snprintf(name, sizeof(name), "%s.b", disk_->prefix);
  if (!blocks_.mount(path,18,nb,name)) return LoadResult::Corrupt;
  std::snprintf(name, sizeof(name), "%s.r", disk_->prefix);
  if (!runs_.mount(path,18+nb*sizeof(Block),nr,name)) return LoadResult::Corrupt;
  std::snprintf(name, sizeof(name), "%s.t", disk_->prefix);
  if (!disk_->text.mount(path,18+nb*sizeof(Block)+nr*sizeof(Run),nt,name)) return LoadResult::Corrupt;
  textLen_ = nt;
  // Validate records in bounded windows before exposing any cursor to layout.
  const auto& blocks = static_cast<const PagedRecords<Block>&>(blocks_);
  const auto& runs = static_cast<const PagedRecords<Run>&>(runs_);
  for (const Block b : blocks) {
    if (static_cast<uint8_t>(b.kind) > static_cast<uint8_t>(BlockKind::Image) || static_cast<uint8_t>(b.align) > 3 ||
        b.runBegin > nr || b.runCount > nr-b.runBegin) return LoadResult::Corrupt;
  }
  for (const Run run : runs) {
    if ((static_cast<uint8_t>(run.style) & ~0x3f) || static_cast<uint8_t>(run.sizeStep) > 4 ||
        run.textLen > kMaxRunBytes || run.textOff > nt || run.textLen > nt-run.textOff) return LoadResult::Corrupt;
  }
  return failed() ? LoadResult::Corrupt : LoadResult::Ok;
}

ChapterIr::LoadResult ChapterIr::loadFromFileEx(const char* path) {
  clear();
  if (!path || !*path) return LoadResult::Corrupt;
  HalFile file;
  if (!Storage.openFileForRead("RVIR", path, file)) return LoadResult::Corrupt;
  char magic[4]; uint16_t ver=0; uint32_t nb=0,nr=0,nt=0;
  if (!serialization::tryReadPod(file,magic) || std::memcmp(magic,kIrMagic,4) ||
      !serialization::tryReadPod(file,ver)) return LoadResult::Corrupt;
  if (ver != kIrFormatVersion) return LoadResult::StaleVersion;
  if (!serialization::tryReadPod(file,nb) || !serialization::tryReadPod(file,nr) || !serialization::tryReadPod(file,nt) ||
      nb > kMaxBlocks || nr > kMaxRuns || nt > kMaxDiskText ||
      18ULL + sizeof(Block)*uint64_t(nb) + sizeof(Run)*uint64_t(nr) + nt != file.size()) return LoadResult::Corrupt;
  file.close();
  const auto result = mountPaged(path,nb,nr,nt);
  if (result != LoadResult::Ok) clear();
  return result;
}

bool ChapterIr::loadFromFile(const char* path) { return loadFromFileEx(path) == LoadResult::Ok; }

int ChapterIr::estimatePageCount(const int viewportW, const int viewportH, const int bodyEmPx,
                                 const float lineCompression) const {
  if (viewportW < 16 || viewportH < 16 || bodyEmPx < 4) return 1;
  if (textLen_ == 0 && blocks_.empty()) return 1;

  if(estimateCached_>=0 && estimateW_==viewportW && estimateH_==viewportH && estimateEm_==bodyEmPx && estimateLc_==lineCompression)return estimateCached_;
  // Heuristic that knows about Rivulet block kinds — not a classic section rebuild,
  // but far closer than "chars / fixed CPL" for chapters with images, HRs, and
  // large headings (the old estimate undercounted plate-heavy spines badly).
  const float lc = lineCompression > 0.1f ? lineCompression : 1.0f;
  const int bodyLine = std::max(bodyEmPx + 2, static_cast<int>(bodyEmPx * 1.2f * lc + 0.5f));
  const int linesPerPage = std::max(1, viewportH / bodyLine);
  // Typical Latin serif advance is ~0.5–0.55em; the old 0.5em (2*W/em) CPL was still
  // too optimistic on e-ink margins and produced first-load ETAs like "7 pages" for
  // a 40-page DCC chapter. Use ~0.62em and floor CPL so we over-estimate slightly
  // (status "~" is better high than low until the idle map catches up).
  const int charsPerLine = std::max(28, (viewportW * 100) / std::max(1, bodyEmPx * 62));

  int contentLines = 0;
  int paraCount = 0;
  for (const Block& b : blocks_) {
    switch (b.kind) {
      case BlockKind::HorizontalRule:
        contentLines += 1;
        break;
      case BlockKind::Spacer: {
        // marginBottomEmQ4 is in 1/16 em.
        const int gap = std::max(1, (static_cast<int>(b.marginBottomEmQ4) * bodyEmPx) / 16);
        contentLines += std::max(1, (gap + bodyLine - 1) / bodyLine);
        break;
      }
      case BlockKind::Image: {
        int h = b.imageH > 0 ? static_cast<int>(b.imageH) : bodyEmPx * 4;
        // Floats share vertical space with wrapping text — charge ~half height.
        if ((b.flags & (kBlockFloatLeft | kBlockFloatRight)) != 0) {
          h = std::max(bodyLine, h / 2);
        }
        // Cap a single plate at one page so a cover-like image cannot explode ETA.
        const int imgLines = std::min(linesPerPage, std::max(1, (h + bodyLine - 1) / bodyLine));
        contentLines += imgLines;
        break;
      }
      default: {
        // Paragraph / heading: count UTF-8 bytes in the block's runs.
        size_t bytes = 0;
        const uint16_t runEnd = static_cast<uint16_t>(b.runBegin + b.runCount);
        for (uint16_t ri = b.runBegin; ri < runEnd && ri < runs_.size(); ++ri) {
          bytes += runs_[ri].textLen;
        }
        int stepBoost = 0;
        if (b.kind == BlockKind::Heading1)
          stepBoost = bodyLine;  // ~extra line of air
        else if (b.kind == BlockKind::Heading2)
          stepBoost = bodyLine / 2;
        else if (b.kind >= BlockKind::Heading3 && b.kind <= BlockKind::Heading6)
          stepBoost = bodyLine / 4;
        // Larger faces use fewer chars per line.
        int cpl = charsPerLine;
        if (b.kind == BlockKind::Heading1)
          cpl = std::max(8, charsPerLine * 2 / 3);
        else if (b.kind == BlockKind::Heading2)
          cpl = std::max(10, charsPerLine * 4 / 5);
        const int textLines =
            bytes == 0 ? 1 : static_cast<int>((bytes + static_cast<size_t>(cpl) - 1) / static_cast<size_t>(cpl));
        contentLines += textLines + (stepBoost + bodyLine - 1) / bodyLine;
        if (b.kind == BlockKind::Paragraph) ++paraCount;
        break;
      }
    }
  }
  contentLines += paraCount / 2;

  // Empty-run chapters (image-only): still at least the image lines above.
  if (contentLines <= 0) {
    contentLines =
        static_cast<int>((textLen_ + static_cast<size_t>(charsPerLine) - 1) / static_cast<size_t>(charsPerLine)) +
        static_cast<int>(blocks_.size()) / 2;
  }

  int pages = std::max(1, (contentLines + linesPerPage - 1) / linesPerPage);
  // Floor from raw text length so a sparse block list cannot under-count a prose chapter.
  if (textLen_ > 0 && charsPerLine > 0 && linesPerPage > 0) {
    const int charsPerPage = charsPerLine * linesPerPage;
    const int fromText =
        static_cast<int>((textLen_ + static_cast<size_t>(charsPerPage) - 1) / static_cast<size_t>(charsPerPage));
    pages = std::max(pages, fromText);
  }
  if (pages == 1 && (textLen_ > 400 || blocks_.size() > 3)) {
    pages = 2;
  }
  // Slight padding while the map is still cold — UI shows "~N"; idle map replaces it.
  if (pages >= 4) {
    pages = pages + std::max(1, pages / 12);
  }
  estimateW_=viewportW;estimateH_=viewportH;estimateEm_=bodyEmPx;estimateLc_=lineCompression;estimateCached_=pages;
  return pages;
}

size_t ChapterIr::serializedSize() const {return 18+blocks_.size()*sizeof(Block)+runs_.size()*sizeof(Run)+textLen_;}
bool ChapterIr::writeRangeTo(HalFile& file,size_t offset,size_t bytes) const {
  if(failed() || offset>serializedSize() || bytes>serializedSize()-offset)return false;
  uint8_t header[18]{};std::memcpy(header,kIrMagic,4);std::memcpy(header+4,&kIrFormatVersion,2);
  const uint32_t nb=blocks_.size(),nr=runs_.size(),nt=textLen_;
  std::memcpy(header+6,&nb,4);std::memcpy(header+10,&nr,4);std::memcpy(header+14,&nt,4);
  alignas(std::max_align_t) uint8_t buffer[512];const size_t blocksEnd=18+nb*sizeof(Block),runsEnd=blocksEnd+nr*sizeof(Run);
  while(bytes){size_t n=std::min(bytes,sizeof(buffer));
    if(offset<18){n=std::min(n,18-offset);std::memcpy(buffer,header+offset,n);}
    else if(offset<blocksEnd){
      const size_t rel=offset-18,index=rel/sizeof(Block),in=rel%sizeof(Block);
      if(!in && n>=sizeof(Block)){const size_t count=std::min(n/sizeof(Block),size_t(nb)-index);n=count*sizeof(Block);
        if(!blocks_.readRange(index,reinterpret_cast<Block*>(buffer),count))return false;}
      else {n=std::min(n,sizeof(Block)-in);const Block b=blocks_[index];std::memcpy(buffer,reinterpret_cast<const uint8_t*>(&b)+in,n);}
    }else if(offset<runsEnd){
      const size_t rel=offset-blocksEnd,index=rel/sizeof(Run),in=rel%sizeof(Run);
      if(!in && n>=sizeof(Run)){const size_t count=std::min(n/sizeof(Run),size_t(nr)-index);n=count*sizeof(Run);
        if(!runs_.readRange(index,reinterpret_cast<Run*>(buffer),count))return false;}
      else {n=std::min(n,sizeof(Run)-in);const Run run=runs_[index];std::memcpy(buffer,reinterpret_cast<const uint8_t*>(&run)+in,n);}
    }else {
      const size_t rel=offset-runsEnd;n=std::min(n,textLen_-rel);
      if(disk_){if(!disk_->text.readRange(rel,reinterpret_cast<char*>(buffer),n))return false;}
      else std::memcpy(buffer,textData_+rel,n);
    }
    if(failed() || file.write(buffer,n)!=n)return false;
    offset+=n;bytes-=n;
  }return true;
}

}  // namespace rivulet
