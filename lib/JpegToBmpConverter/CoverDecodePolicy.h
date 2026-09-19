#pragma once

#include <cstddef>
#include <cstdint>

// Home/sleep jacket JPEG. Field crash_report + log_00001 (Parade of Horribles,
// 944×1504 progressive):
//   jpgd NOTENOUGHMEM (-225) ~8s, then JPEGDEC 1/8 with decode(0,0,0) → err=2,
//   then the same JPEG decoded again via OPF reparse (17s LOOP, rst=6 TASK_WDT).
namespace coverdecode {

// bitbank2 JPEGDEC JPEG_SCALE_EIGHTH. In-chapter progressive images pass this
// into JPEGDEC::decode(); the cover path used 0, so MCU callbacks arrived at
// full-res coordinates against a 1/8 grid and decode failed (err=2).
inline constexpr int kJpegScaleEighth = 8;

inline constexpr int jpegDecDecodeOptions(const bool progressive) {
  return progressive ? kJpegScaleEighth : 0;
}

// jpgd+spill needs two ~15 KB AC rows plus decoder tables. X3 Home after a
// FrameBufferLoan still reports maxAlloc≈69 KB — every field attempt OOMs.
inline constexpr unsigned kJpgdMinMaxAllocBytes = 96u * 1024u;

inline constexpr bool useFullProgressiveDecode(const unsigned maxAllocBytes) {
  return maxAllocBytes >= kJpgdMinMaxAllocBytes;
}

// generateThumbBmp used to decode the same cover.jpg twice (book.bin href,
// then OPF). Skip the second pass when the path did not change.
inline constexpr bool skipSameCoverHrefRetry(const char* cachedHref, const char* freshHref) {
  if (!freshHref || freshHref[0] == '\0') return true;
  if (!cachedHref || cachedHref[0] == '\0') return false;
  std::size_t i = 0;
  while (cachedHref[i] != '\0' && cachedHref[i] == freshHref[i]) ++i;
  return cachedHref[i] == '\0' && freshHref[i] == '\0';
}

}  // namespace coverdecode
