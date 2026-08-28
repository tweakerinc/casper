#include <gtest/gtest.h>

#include "util/CachedIrPolicy.h"

namespace {

// The rule ChapterLoader used to apply to a successfully loaded IR.
bool oldShortVsHtml(const size_t htmlSz, const size_t textSz) {
  return (htmlSz > 8000 && textSz > 0 && textSz * 5 / 2 < htmlSz) || (htmlSz > 20000 && textSz < 5000);
}

}  // namespace

TEST(CachedIrPolicy, TypicalEpubChapterHtmlIsNotTruncation) {
  // ~3× markup overhead: a chapter the user just finished reading.
  constexpr size_t kHtml = 50000;
  constexpr size_t kText = 15000;
  EXPECT_TRUE(oldShortVsHtml(kHtml, kText));
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(kHtml, kText));
}

TEST(CachedIrPolicy, ShortProseWithFatXhtmlIsNotTruncation) {
  // Cover/front-matter: lots of CSS, little body text.
  constexpr size_t kHtml = 24000;
  constexpr size_t kText = 4000;
  EXPECT_TRUE(oldShortVsHtml(kHtml, kText));
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(kHtml, kText));
}

TEST(CachedIrPolicy, EqualSizesStayAccepted) {
  EXPECT_FALSE(cachedir::rejectLoadedIrForHtmlRatio(8000, 8000));
  EXPECT_FALSE(oldShortVsHtml(8000, 8000));
}

TEST(CachedIrPolicy, OomDoesNotDeleteTheFile) { EXPECT_FALSE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::Oom)); }

TEST(CachedIrPolicy, CorruptHeaderMayDelete) {
  EXPECT_TRUE(cachedir::deleteFileOnLoadMiss(cachedir::LoadMiss::Corrupt));
}
