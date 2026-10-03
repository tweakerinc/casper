#pragma once
#include <cstdint>
namespace rivulet::preparationbudget {
// The existing ZIP inflater needs a contiguous ~43 KiB arena in addition to
// I/O buffers. A 16 KiB largest-block gate admitted work which could never run.
inline constexpr uint32_t kStartFree = 80U * 1024U;
inline constexpr uint32_t kStartLargest = 48U * 1024U;
inline constexpr uint32_t kQuietMs = 2500;
inline constexpr uint32_t kBetweenSlicesMs = 80;
inline bool canStart(uint32_t freeBytes,uint32_t largest) {
  return freeBytes>=kStartFree && largest>=kStartLargest;
}
inline bool isQuiet(uint32_t now,uint32_t lastTurn,uint32_t firstInk) {
  return uint32_t(now-lastTurn)>=kQuietMs && uint32_t(now-firstInk)>=kQuietMs;
}
}
