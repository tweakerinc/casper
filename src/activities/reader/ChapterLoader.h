#pragma once

#include <Epub.h>
#include <RivuletEngine.h>

#include <cstdint>
#include <string>
#include <memory>

class GfxRenderer;

// Full-source chapter acquisition. Cached IR and newly converted HTML use
// bounded SD-backed record/text windows. The caller owns the target engine;
// loading into a worker never evicts the active reader. No prefix is accepted
// as a complete chapter, and image sizing uses the same callback as painting.
namespace chapterload {

// Optional owner hooks; image geometry is required for faithful page counts.
struct Hooks {
  void* ctx = nullptr;
  void (*prepareHeap)(void* ctx, bool aggressive) = nullptr;
  void (*prepareImages)(void* ctx, const char* href) = nullptr;
  // Optional. When true, abandon a convert in progress so a tap is not stuck
  // behind a 10s+ HTML ingest. Caller must restore any evicted chapter.
  bool (*shouldAbort)(void* ctx) = nullptr;
};

struct Request {
  Epub* epub = nullptr;
  rivulet::RivuletEngine* engine = nullptr;
  GfxRenderer* renderer = nullptr;
  std::string irDir;  // <book>/rivulet
  int spineIndex = 0;
  uint8_t imageRendering = 0;  // CrossPointSettings::IMAGE_RENDERING
  // Refuse a partial (OOM-truncated) convert. Used where a false chapter end
  // would be actively wrong, e.g. seeking the true last page of a chapter.
  bool requireCompleteIr = false;
  // Bind the engine's laid-out-page cache to this spine. Off for indexing, which
  // never paints and so would only write files nobody reads.
  bool bindPageCache = true;
  // Lend the framebuffer's 48 KB to cache deserialize AND convert.
  // MUST be false when the caller's screen is still on the panel and will do
  // windowed repaints afterwards: the loan hands the buffer back white, so a
  // later partial update paints blank over live UI. Sitting loadSpine leaves
  // this true so a CrossPoint .rvir can load instead of flashing
  // "Chapter not readable".
  bool lendFrameBuffer = false;
};

struct Result {
  bool empty = false;      // verified empty source, not a failed load
  bool ok = false;         // chapter IR is loaded and usable
  bool fromCache = false;  // came from .rvir rather than a fresh convert
  bool partial = false;    // convert hit a cap/OOM; IR is truncated
};

class Session {
 public:
  enum class Status {Working,Done,Failed,Cancelled};
  explicit Session(const Request& req,const Hooks& hooks={});
  ~Session();
  Session(const Session&)=delete;
  Session& operator=(const Session&)=delete;
  Status step();
  const Result& result()const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Result loadChapterIr(const Request& req, const Hooks& hooks);

}  // namespace chapterload
