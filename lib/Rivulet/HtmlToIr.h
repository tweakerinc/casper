#pragma once

#include "ChapterIr.h"

namespace rivulet {

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
  // 2=Suppress (omit img entirely). Matches CasperSettings::IMAGE_RENDERING.
  // Font glyphs and text drop-caps are unrelated — this only affects <img>/ornaments.
  static bool convert(const char* html, size_t len, ChapterIr& out, bool armDropCapOnFirstParagraph = false,
                      uint8_t imageRendering = 0);

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
