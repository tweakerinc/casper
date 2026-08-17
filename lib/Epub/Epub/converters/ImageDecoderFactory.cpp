#include "ImageDecoderFactory.h"

#include <Logging.h>
#include <Memory.h>

#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  // nothrow throughout: bare `new` aborts under -fno-exceptions, and decoders are
  // constructed lazily on the image-paint path where the heap is tightest.
  // Callers already handle a null decoder (the "no decoder found" path below).
  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      if (!jpegDecoder) {
        LOG_ERR("DEC", "OOM: JPEG decoder for %s", imagePath.c_str());
        return nullptr;
      }
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      if (!pngDecoder) {
        LOG_ERR("DEC", "OOM: PNG decoder for %s", imagePath.c_str());
        return nullptr;
      }
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
