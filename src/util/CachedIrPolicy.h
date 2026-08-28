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
// 2. loadIr failure: deserialize is one contiguous malloc of the text blob.
//    Tight maxAlloc after a menu is OOM, not a corrupt file. Deleting the
//    .rvir then forced a heavier HTML convert that also failed.
namespace cachedir {

enum class LoadMiss : uint8_t {
  Oom = 0,
  Corrupt = 1,
};

// Never reject a deserialized IR because HTML is larger than text.
inline constexpr bool rejectLoadedIrForHtmlRatio(const size_t htmlBytes, const size_t textBytes) {
  (void)htmlBytes;
  (void)textBytes;
  return false;
}

// Delete the file only when the header is actually unreadable. Keep it on OOM
// so the next scrub can load the chapter the user already read.
inline constexpr bool deleteFileOnLoadMiss(const LoadMiss miss) { return miss == LoadMiss::Corrupt; }

}  // namespace cachedir
