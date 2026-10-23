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

// Admission is based on the largest actual row allocation and TOTAL decoder
// working memory, not a fictitious 96 KiB single allocation. The strict-SD
// field fixture used <58 KiB decoder heap with a 15,104-byte largest request
// on the host; reserve additional memory for BMP scaling and other tasks.
inline constexpr unsigned kJpgdMinMaxAllocBytes = 32u * 1024u;
inline constexpr unsigned kJpgdMinFreeBytes = 96u * 1024u;

inline constexpr bool useFullProgressiveDecode(unsigned maxAllocBytes,unsigned freeBytes) {
  return maxAllocBytes >= kJpgdMinMaxAllocBytes && freeBytes >= kJpgdMinFreeBytes;
}

// RC3 routed every progressive jacket through jpgd+spill and returned false when
// the 96 KB free gate missed. After reading, maxAlloc is often 30–73 KB even
// with ~100 KB free — Home then treated that as a no-progress decoder failure
// and stopped retrying, so covers never appeared. JPEGDEC 1/8 is the path that
// actually ran on X3 (CoverRenderPolicy). Use it when jpgd cannot start.
// jpegDecDecodeOptions(progressive)=8 so MCU coords match the 1/8 grid (err=2).
inline constexpr bool jpegDecEighthFallback(unsigned maxAllocBytes, unsigned freeBytes) {
  return !useFullProgressiveDecode(maxAllocBytes, freeBytes);
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
