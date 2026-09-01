#pragma once

#include <cstdint>

// Home cover BMPs live on SD. Returning from the reader must blit a cached
// thumb, never decode JPEG, even if the 1:1 560 file is missing and only a
// smaller leftover exists.
namespace thumbcache {

// Keep a cached thumb unless we opened it and it is not a BMP.
// exists() can false-negative after the reader: an open that returns a BMP wins.
// Open failed: keep if exists() said the file is there (SD busy, not corrupt).
inline bool keepExistingThumb(const bool exists, const bool opened, const bool validBmp) {
  if (opened) return validBmp;
  return exists;
}

inline bool deleteCorruptThumb(const bool exists, const bool opened, const bool validBmp) {
  return exists && opened && !validBmp;
}

enum class DiskThumb : uint8_t { Hero, Fallback, Missing };

inline DiskThumb classify(const bool heroExists, const bool fallbackExists) {
  if (heroExists) return DiskThumb::Hero;
  if (fallbackExists) return DiskThumb::Fallback;
  return DiskThumb::Missing;
}

// Any on-disk thumb is enough to paint Home. JPEG is only for a first-ever miss.
inline bool skipJpeg(const DiskThumb state) { return state != DiskThumb::Missing; }

inline bool jpegWhenIdle(const DiskThumb state) { return state == DiskThumb::Missing; }

}  // namespace thumbcache
