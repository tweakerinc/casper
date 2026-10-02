#include <gtest/gtest.h>

#include "CoverDecodePolicy.h"

TEST(CoverDecodePolicy, ProgressiveUsesJpegScaleEighth) {
  EXPECT_EQ(coverdecode::jpegDecDecodeOptions(/*progressive=*/true), 8);
  EXPECT_EQ(coverdecode::jpegDecDecodeOptions(/*progressive=*/false), 0);
}

TEST(CoverDecodePolicy, SkipsJpgdWhenC3MaxAllocIsFieldSized) {
  EXPECT_FALSE(coverdecode::useFullProgressiveDecode(69620));
  EXPECT_FALSE(coverdecode::useFullProgressiveDecode(coverdecode::kJpgdMinMaxAllocBytes - 1));
  EXPECT_TRUE(coverdecode::useFullProgressiveDecode(coverdecode::kJpgdMinMaxAllocBytes));
  EXPECT_TRUE(coverdecode::useFullProgressiveDecode(128u * 1024u));
}

TEST(CoverDecodePolicy, SkipsSecondDecodeWhenHrefUnchanged) {
  EXPECT_TRUE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", "OEBPS/cover.jpg"));
  EXPECT_FALSE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", "OEBPS/cover.jpeg"));
  EXPECT_FALSE(coverdecode::skipSameCoverHrefRetry("", "OEBPS/cover.jpg"));
  EXPECT_TRUE(coverdecode::skipSameCoverHrefRetry("OEBPS/cover.jpg", ""));
}
