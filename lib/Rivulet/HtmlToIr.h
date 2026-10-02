#pragma once

#include "ChapterIr.h"

namespace rivulet {
// A continuous parser with explicit yield points. Owns only an input window
// and bounded parser state; caller keeps file/chapter alive until destruction.
class HtmlToIrSession {
 public:
  enum class Result {Working,Done,Failed};
  HtmlToIrSession(HalFile& file,const char* workPath,ChapterIr& out,bool arm=false,uint8_t images=0,
                  bool(*cancel)(void*)=nullptr,void*ctx=nullptr);
  ~HtmlToIrSession();
  HtmlToIrSession(const HtmlToIrSession&)=delete;
  HtmlToIrSession& operator=(const HtmlToIrSession&)=delete;
  Result step(size_t byteBudget=4096);
  size_t consumed()const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};


// Streaming-ish XHTML → ChapterIr for Latin EPUB body content.
// Intentionally small: tags we care about for book feel; rest stripped.
//
// Supports: p, h1–h6, br, hr, b/strong, i/em, div/blockquote (drop-cap host classes),
//           ol/ul/li/nav (TOC & lists — each li is its own paragraph),
//           span (style inheritance only), text nodes, basic entities.
// Ignores: script/style/svg, tables (inner text kept loosely), RTL attrs.
// Skips: class/attr/style hidden hosts (oculto, HTML hidden=, display:none).
class HtmlToIr {
 public:
  // imageRendering: 0=Display (raster plates), 1=Placeholder (alt as "[Image: …]"),
  // 2=Suppress (omit img entirely). Matches CrossPointSettings::IMAGE_RENDERING.
  // Font glyphs and text drop-caps are unrelated — this only affects <img>/ornaments.
  static bool convert(const char* html, size_t len, ChapterIr& out, bool armDropCapOnFirstParagraph = false,
                      uint8_t imageRendering = 0);

  // Continuous conversion into SD-backed IR, without a chapter-sized buffer.
  static bool convertFile(HalFile& file, const char* workPath, ChapterIr& out,
                          bool armDropCapOnFirstParagraph = false, uint8_t imageRendering = 0,
                          bool (*cancel)(void*) = nullptr, void* ctx = nullptr);

  // Convenience: null-terminated.
  static bool convert(const char* html, ChapterIr& out, bool armDropCapOnFirstParagraph = false,
                      uint8_t imageRendering = 0) {
    if (!html) return false;
    size_t n = 0;
    while (html[n]) ++n;
    return convert(html, n, out, armDropCapOnFirstParagraph, imageRendering);
  }
};

}  // namespace rivulet
