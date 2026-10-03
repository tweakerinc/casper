#include <gtest/gtest.h>

#include "CoverDecodePolicy.h"

TEST(CoverDecodePolicy, ProgressiveUsesJpegScaleEighth) {
  EXPECT_EQ(coverdecode::jpegDecDecodeOptions(/*progressive=*/true), 8);
  EXPECT_EQ(coverdecode::jpegDecDecodeOptions(/*progressive=*/false), 0);
}

TEST(CoverDecodePolicy, DistinguishesTotalBudgetFromLargestAllocation) {
  EXPECT_TRUE(coverdecode::useFullProgressiveDecode(63476,109924));
  EXPECT_TRUE(coverdecode::useFullProgressiveDecode(45044,104272));
  EXPECT_FALSE(coverdecode::useFullProgressiveDecode(16000,109924));
  EXPECT_FALSE(coverdecode::useFullProgressiveDecode(63476,40000));
  EXPECT_TRUE(coverdecode::useFullProgressiveDecode(coverdecode::kJpgdMinMaxAllocBytes,coverdecode::kJpgdMinFreeBytes));
  EXPECT_FALSE(coverdecode::useFullProgressiveDecode(coverdecode::kJpgdMinMaxAllocBytes-1,coverdecode::kJpgdMinFreeBytes));
}

TEST(CoverDecodePolicy, SkipsSecondDecodeWhenHrefUnchanged) {
  EXPECT_TRUE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", "OEBPS/cover.jpg"));
  EXPECT_FALSE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", "OEBPS/cover.jpeg"));
  EXPECT_FALSE(coverdecode::skipSameCoverHrefRetry("", "OEBPS/cover.jpg"));
  EXPECT_TRUE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", ""));
}
