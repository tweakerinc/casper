#include <gtest/gtest.h>

#include "ChapterLoadPolicy.h"

TEST(ChapterLoadPolicy, CacheOomStillConverts) { EXPECT_TRUE(chapterloadpolicy::convertAfterCacheOom()); }

TEST(ChapterLoadPolicy, CacheDeserializeUsesFramebufferLoan) {
  EXPECT_TRUE(chapterloadpolicy::loanFramebufferForCache());
}

TEST(ChapterLoadPolicy, OversizedHtmlConvertsPrefix) {
  EXPECT_FALSE(chapterloadpolicy::skipOversizedHtml());
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(200 * 1024, chapterloadpolicy::kMaxHtmlInRam, 80 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            64 * 1024);
}

TEST(ChapterLoadPolicy, FileThatFitsKeepsFullHtml) {
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(10 * 1024, chapterloadpolicy::kMaxHtmlInRam, 80 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            10 * 1024);
}

TEST(ChapterLoadPolicy, CapIsMaxHtmlInRam) {
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(200 * 1024, chapterloadpolicy::kMaxHtmlInRam, 200 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            chapterloadpolicy::kMaxHtmlInRam);
}

TEST(ChapterLoadPolicy, TightHeapStillConvertsPrefix) {
  // leftover 8 KB is under the 16 KB headroom. The old loader skipped convert
  // (maxA < 12 KB after HTML). Sitting open must still ingest a prefix.
  EXPECT_EQ(chapterloadpolicy::kSkipConvertIfLeftoverBelow, 0u);
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(50 * 1024, chapterloadpolicy::kMaxHtmlInRam, 8 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            4 * 1024);
}

TEST(ChapterLoadPolicy, RoomAfterHeadroomShrinksHtml) {
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(50 * 1024, chapterloadpolicy::kMaxHtmlInRam, 20 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            4 * 1024);
}

TEST(ChapterLoadPolicy, EmptyHtmlOrHeapYieldsNothing) {
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(0, chapterloadpolicy::kMaxHtmlInRam, 80 * 1024,
                                                  chapterloadpolicy::kConvertHeadroom),
            0u);
  EXPECT_EQ(chapterloadpolicy::htmlBytesToConvert(50 * 1024, chapterloadpolicy::kMaxHtmlInRam, 0,
                                                  chapterloadpolicy::kConvertHeadroom),
            0u);
}

TEST(ChapterLoadPolicy, SittingOpenRetriesConvert) {
  EXPECT_TRUE(chapterloadpolicy::extraConvertRetry(/*sittingOpen=*/true, /*requireCompleteIr=*/false));
  EXPECT_TRUE(chapterloadpolicy::extraConvertRetry(/*sittingOpen=*/false, /*requireCompleteIr=*/true));
  EXPECT_FALSE(chapterloadpolicy::extraConvertRetry(/*sittingOpen=*/false, /*requireCompleteIr=*/false));
}

TEST(ChapterLoadPolicy, SiblingImageModesWrap) {
  EXPECT_TRUE(chapterloadpolicy::trySiblingImageModeCaches());
  EXPECT_EQ(chapterloadpolicy::cacheModeToTry(2, 0), 2);
  EXPECT_EQ(chapterloadpolicy::cacheModeToTry(2, 1), 0);
  EXPECT_EQ(chapterloadpolicy::cacheModeToTry(2, 2), 1);
  EXPECT_EQ(chapterloadpolicy::cacheModeToTry(0, 1), 1);
}
