#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  // coverThumb: 2-bit balanced Atkinson home cover (match JPEG c22).
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool floydSteinberg = false,
                                         bool coverThumb = false);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Home covers: 2-bit balanced Atkinson + mild lift (match JPEG c22).
  static bool pngFileToCoverThumbBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight);
};
