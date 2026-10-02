#pragma once

#include <cstddef>
#include <cstdint>

// Policy for on-disk chapter IR (.rvir) after a load attempt.
//
// A chapter the user just finished reading is not "bad." The loader used to
// throw those caches away and reconvert, which OOMs on X3:
//
// 1. HTML-vs-text ratio: EPUB XHTML is markup. 50 KB of HTML with 15 KB of
//    extracted text is normal (tags, CSS, class attributes, SVG). The old
//    2.5× / "html>20KB and text<5KB" test treated that as a truncated convert.
// 3. version mismatch: v19–v26 share one on-disk layout. Treating an older
//    CrossPoint/CrossInk cache as corrupt deleted it and forced a convert that
//    abort()ed ("Chapter not readable").
namespace cachedir {

enum class LoadMiss : uint8_t {
  Oom = 0,
  Corrupt = 1,
  StaleVersion = 2,
};

// Never reject a deserialized IR because HTML is larger than text.
inline constexpr bool rejectLoadedIrForHtmlRatio(const size_t htmlBytes, const size_t textBytes) {
  (void)htmlBytes;
  (void)textBytes;
  return false;
}

// v19–v26 share one on-disk layout. A parser bump must still load older files.
inline constexpr bool irVersionLoadable(const uint16_t ver, const uint16_t minV, const uint16_t maxV) {
  return ver >= minV && ver <= maxV;
}

// Delete only when the header is actually unreadable. Keep the file on OOM and
// on a version we do not load yet — converting under a tight heap used to
// abort() and leave every chapter as "Chapter not readable".
inline constexpr bool deleteFileOnLoadMiss(const LoadMiss miss) { return miss == LoadMiss::Corrupt; }

}  // namespace cachedir
