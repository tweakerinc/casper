#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "DitherUtils.h"

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  // Plate = photos/art (lift). Ink = drop-cap letters (darken).
  // Cover = document text-in-JPEG / Illuminae briefings (cover-thumb midtones,
  // Bayer only — no Atkinson multipass like home cover gen).
  EinkImageTone tone = EinkImageTone::Plate;
  // Legacy alias: true → Ink, false leaves tone as set (default Plate).
  bool inkBias = false;
  // When false, decoders only populate cachePath (.pxc) and never touch the
  // live framebuffer. Layout precache runs mid-SAX under a deep stack; writing
  // + white-out of the FB was unnecessary and has been a crash vector when the
  // heap is already fragmented after TextSettings reflow.
  bool writeToFramebuffer = true;
  std::string cachePath;  // If non-empty, decoder will write pixel cache to this path

  EinkImageTone resolvedTone() const {
    if (inkBias) return EinkImageTone::Ink;
    return tone;
  }
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
