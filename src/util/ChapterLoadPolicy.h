#pragma once

#include <cstddef>
#include <cstdint>

// Sitting chapter open must produce IR whenever the EPUB has HTML.
//
// "Chapter not readable" was not a bad EPUB. The loader hard-failed after a
// cache miss that it caused:
//
// 1. Cache deserialize ran without the 48 KB framebuffer loan, so a 50 KB
//    .rvir OOMed at maxAlloc ~11–48 KB.
// 2. Cache OOM returned fail and skipped convert ("keep cache, skip convert").
// 3. HTML larger than 160 KB, or leftover heap under 12 KB after the HTML
//    buffer, skipped convert entirely instead of ingesting a prefix.
// 4. Only requireCompleteIr got a third convert attempt. A sitting Next/open
//    did not.
//
// Partial IR is readable. Empty/fail is not. Idle/Home indexing may still
// refuse the framebuffer loan (panel still shows the current page).
namespace chapterloadpolicy {

inline constexpr size_t kMaxHtmlInRam = 160 * 1024;
inline constexpr size_t kConvertHeadroom = 16 * 1024;
inline constexpr uint8_t kImageModeCount = 3;

// Never skip convert because leftover heap looks "too small".
inline constexpr size_t kSkipConvertIfLeftoverBelow = 0;

inline constexpr bool loanFramebufferForCache() { return true; }

inline constexpr bool convertAfterCacheOom() { return true; }

inline constexpr bool skipOversizedHtml() { return false; }

inline constexpr bool trySiblingImageModeCaches() { return true; }

// Sitting open (framebuffer loan allowed) retries convert after a hard scrub.
// Idle/Home indexing does not, unless the caller required a complete IR.
inline constexpr bool extraConvertRetry(const bool sittingOpen, const bool requireCompleteIr) {
  return sittingOpen || requireCompleteIr;
}

// Image-mode slot to try: 0 is the request's mode, then wrap 0/1/2.
inline constexpr uint8_t cacheModeToTry(const uint8_t requestedMode, const uint8_t tryIndex,
                                        const uint8_t modeCount = kImageModeCount) {
  if (modeCount == 0) return requestedMode;
  return static_cast<uint8_t>((static_cast<unsigned>(requestedMode) + tryIndex) % modeCount);
}

// Bytes of chapter HTML to hold in RAM for convert. Files larger than
// kMaxHtmlInRam still convert a prefix — never skip the chapter.
// leftoverHeap is contiguous heap before the HTML allocation (maxAlloc).
inline constexpr size_t htmlBytesToConvert(const size_t fileSize, const size_t maxHtmlInRam, const size_t leftoverHeap,
                                           const size_t convertHeadroom) {
  if (fileSize == 0 || leftoverHeap == 0) return 0;
  size_t cap = fileSize;
  if (cap > maxHtmlInRam) cap = maxHtmlInRam;
  size_t room = leftoverHeap;
  if (leftoverHeap > convertHeadroom) {
    room = leftoverHeap - convertHeadroom;
  } else {
    room = leftoverHeap / 2;
    if (room == 0) room = leftoverHeap;
  }
  if (cap > room) cap = room;
  return cap;
}

}  // namespace chapterloadpolicy
