#include <gtest/gtest.h>

#include "Rivulet/IrFormat.h"
#include "util/CachedIrPolicy.h"

namespace {

// The rule ChapterLoader used to apply to a successfully loaded IR.
bool oldShortVsHtml(const size_t htmlSz, const size_t textSz) {
  return (htmlSz > 8000 && textSz > 0 && textSz * 5 / 2 < htmlSz) || (htmlSz > 20000 && textSz < 5000);
}

}  // namespace

TEST(CachedIrPolicy, TypicalEpubChapterHtmlIsNotTruncation) {
  constexpr size_t kHtml = 50000;
  constexpr size_t kText = 15000;
  EXPECT_TRUE(oldShortVsHtml(kHtml, kText));
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(kHtml, kText));
}

TEST(CachedIrPolicy, ShortProseWithFatXhtmlIsNotTruncation) {
  constexpr size_t kHtml = 24000;
  constexpr size_t kText = 4000;
  EXPECT_TRUE(oldShortVsHtml(kHtml, kText));
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(kHtml, kText));
}

TEST(CachedIrPolicy, EqualSizesStayAccepted) {
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(8000, 8000));
  EXPECT_FALSE(oldShortVsHtml(8000, 8000));
}

TEST(CachedIrPolicy, OomDoesNotDeleteTheFile) {
  EXPECT_FALSE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::Oom));
}

TEST(CachedIrPolicy, CorruptHeaderMayDelete) {
  EXPECT_TRUE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::Corrupt));
}

TEST(CachedIrPolicy, StaleVersionDoesNotDeleteTheFile) {
  EXPECT_FALSE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::StaleVersion));
}

TEST(CachedIrPolicy, LegacyPrefixWithoutCompletionProofMustReconvert) {
  // v19-v26 can contain an unmarked prefix; v27-v28 can contain flattened
  // div content. Rebuild derived data for the corrected v29 parser.
  // Rebuild derived IR once; do not remove the source or user progress.
  EXPECT_EQ(rivulet::kIrFormatVersionMin, 29);
  EXPECT_EQ(rivulet::kIrFormatVersionMax, rivulet::kIrFormatVersion);
  for (uint16_t v = 19; v <= 28; ++v) {
    EXPECT_FALSE(cachedir::irVersionLoadable(v, rivulet::kIrFormatVersionMin,
                                            rivulet::kIrFormatVersionMax));
  }
  EXPECT_TRUE(cachedir::irVersionLoadable(29, rivulet::kIrFormatVersionMin,
                                         rivulet::kIrFormatVersionMax));
  EXPECT_FALSE(cachedir::irVersionLoadable(18, rivulet::kIrFormatVersionMin,
                                          rivulet::kIrFormatVersionMax));
  EXPECT_FALSE(cachedir::irVersionLoadable(rivulet::kIrFormatVersionMax + 1,
                                          rivulet::kIrFormatVersionMin,
                                          rivulet::kIrFormatVersionMax));
  EXPECT_FALSE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::StaleVersion));
}
