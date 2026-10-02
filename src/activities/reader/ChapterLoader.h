#pragma once

#include <Epub.h>
#include <RivuletEngine.h>

#include <cstdint>
#include <string>

class GfxRenderer;

// Getting one chapter's IR into a RivuletEngine.
//
// Sitting open (lendFrameBuffer=true) must produce IR when the EPUB has HTML.
// Partial text is readable; returning fail is what flashed "Chapter not readable".
//
// Pipeline:
//   framebuffer loan (cache + convert) so a 50 KB .rvir can deserialize
//   cached .rvir for this Images mode, then sibling sN_m{0,1,2}.rvir
//   else ZIP inflate -> SD -> prefix of HTML that fits -> HtmlToIr
//        -> persist only a complete IR
//
// A successfully loaded .rvir is trusted. HTML is markup, so html>>text is
// normal EPUB, not a truncated convert (see util/CachedIrPolicy.h,
// util/ChapterLoadPolicy.h).
//
// It is separate from the reader activity so other callers can use it — the Home
// screen indexes chapters while no book is open, where nothing has to be evicted
// to make room and free heap is at its highest. Doing that from inside the reader
// meant swapping the resident chapter out from under someone who was reading.
//
// Deliberately does NOT lay out or paint anything: callers decide what to do with
// the loaded chapter (show a page, walk a page map, ...).
namespace chapterload {

// Callbacks into the owner. Heap preparation is required — every caller has its
// own idea of what may be thrown away. Image preparation is optional: it probes
// image dimensions for the current viewport, which a background indexer does not
// need because it never paints.
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
  bool lendFrameBuffer = true;
};

struct Result {
  bool ok = false;         // chapter IR is loaded and usable
  bool fromCache = false;  // came from .rvir rather than a fresh convert
  bool partial = false;    // convert hit a cap/OOM; IR is truncated
};

Result loadChapterIr(const Request& req, const Hooks& hooks);

}  // namespace chapterload
