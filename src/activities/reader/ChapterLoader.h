#pragma once

#include <Epub.h>
#include <RivuletEngine.h>

#include <cstdint>
#include <string>

class GfxRenderer;

// Getting one chapter's IR into a RivuletEngine.
//
// This is the pipeline that used to live inside RivuletReaderActivity::loadSpine:
//   cached .rvir (validated against the on-disk HTML)  ->  page map load + scrub
//   else  ZIP inflate -> SD -> read into RAM under a framebuffer loan -> HtmlToIr
//         -> persist IR, drop any map built from an older/partial convert
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
};

struct Request {
  Epub* epub = nullptr;
  rivulet::RivuletEngine* engine = nullptr;
  GfxRenderer* renderer = nullptr;
  std::string irDir;         // <book>/rivulet
  int spineIndex = 0;
  uint8_t imageRendering = 0;  // CasperSettings::IMAGE_RENDERING
  // Refuse a partial (OOM-truncated) convert. Used where a false chapter end
  // would be actively wrong, e.g. seeking the true last page of a chapter.
  bool requireCompleteIr = false;
  // Bind the engine's laid-out-page cache to this spine. Off for indexing, which
  // never paints and so would only write files nobody reads.
  bool bindPageCache = true;
};

struct Result {
  bool ok = false;          // chapter IR is loaded and usable
  bool fromCache = false;   // came from .rvir rather than a fresh convert
  bool partial = false;     // convert hit a cap/OOM; IR is truncated
};

Result loadChapterIr(const Request& req, const Hooks& hooks);

}  // namespace chapterload
