#pragma once

#include <cstdint>

// Home cover BMPs live on SD. Returning from the reader must blit a cached
// thumb, never decode JPEG, even if the 1:1 560 file is missing and only a
// smaller leftover exists.
namespace thumbcache {

// Header-only / truncated leftovers from a crashed JPEG write used to pass
// "BM" + size>62 and lock the cache forever (Bare then blits a white plate).
// Smallest real shelf thumb is a few KB; 1 KB still rejects a 70-byte header.
inline constexpr unsigned kMinThumbBytes = 1024;

inline constexpr bool validBmpProbe(const unsigned bytesRead, const char s0, const char s1, const unsigned fileBytes) {
  return bytesRead == 2 && s0 == 'B' && s1 == 'M' && fileBytes >= kMinThumbBytes;
}

// generateThumbBmp: keep a file we could not open if exists() says it is there
// (SD busy after the reader). Truncating that path would destroy a good jacket.
// Paint/classify must still require an opened valid BMP — see DiskThumb::Unverified.
inline constexpr bool keepExistingThumb(const bool exists, const bool opened, const bool validBmp) {
  if (opened) return validBmp;
  return exists;
}

inline constexpr bool deleteCorruptThumb(const bool exists, const bool opened, const bool validBmp) {
  return exists && opened && !validBmp;
}

enum class DiskThumb : uint8_t { Hero, Fallback, Unverified, Missing };

// heroValid / fallbackValid: opened a real BMP. unverified: a candidate path
// exists or opened but was not a paintably valid BMP (SD busy, not yet deleted).
inline constexpr DiskThumb classify(const bool heroValid, const bool fallbackValid, const bool unverified = false) {
  if (heroValid) return DiskThumb::Hero;
  if (fallbackValid) return DiskThumb::Fallback;
  if (unverified) return DiskThumb::Unverified;
  return DiskThumb::Missing;
}

// Any on-disk thumb is enough to skip JPEG on Back — including Unverified, so
// a busy SD cannot hitch a 10–30s decode. Idle may still generate a missing
// 1:1 hero (see coverrender::generateHero) with a "Rendering Cover" cue.
inline constexpr bool skipJpeg(const DiskThumb state) { return state != DiskThumb::Missing; }

inline constexpr bool jpegWhenIdle(const DiskThumb state) { return state == DiskThumb::Missing; }

}  // namespace thumbcache
