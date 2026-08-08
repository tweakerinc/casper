#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  // Allocate the shared PNGdec instance early (section parse / image extract)
  // while the heap is still contiguous. Paint-time lazy alloc often fails once
  // font slabs have fragmented maxAlloc below ~55 KB.
  static bool warmSharedDecoder();
  // Hold the shared PNGdec across a multi-image section build so Alice-style
  // chapters don't thrash ~50 KB alloc/free per figure. Nested begin/end ok.
  // Skips holding when heap is already tight (C3: ~50 KB PNGdec starves word
  // vectors → empty pages / missing text). Nested begin/end ok.
  static void beginSectionWarm();
  static void endSectionWarm();
  // Drop a held section-warm decoder when maxAlloc collapses mid-parse so
  // ParsedText / ZIP / fonts can grow again.
  static void releaseWarmIfHeapTight(size_t minMaxAllocBytes = 20 * 1024);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "PNG"; }
};