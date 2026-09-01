#include <gtest/gtest.h>

#include "ThumbCachePolicy.h"

TEST(ThumbCachePolicy, KeepWhenProbeCannotOpen) {
  EXPECT_TRUE(thumbcache::keepExistingThumb(/*exists=*/true, /*opened=*/false, /*validBmp=*/false));
  EXPECT_FALSE(thumbcache::deleteCorruptThumb(/*exists=*/true, /*opened=*/false, /*validBmp=*/false));
}

TEST(ThumbCachePolicy, OpenSuccessWinsOverExistsFalse) {
  EXPECT_TRUE(thumbcache::keepExistingThumb(/*exists=*/false, /*opened=*/true, /*validBmp=*/true));
  EXPECT_FALSE(thumbcache::keepExistingThumb(/*exists=*/false, /*opened=*/false, /*validBmp=*/false));
}

TEST(ThumbCachePolicy, KeepValidBmp) {
  EXPECT_TRUE(thumbcache::keepExistingThumb(/*exists=*/true, /*opened=*/true, /*validBmp=*/true));
  EXPECT_FALSE(thumbcache::deleteCorruptThumb(/*exists=*/true, /*opened=*/true, /*validBmp=*/true));
}

TEST(ThumbCachePolicy, DeleteOpenedNonBmp) {
  EXPECT_FALSE(thumbcache::keepExistingThumb(/*exists=*/true, /*opened=*/true, /*validBmp=*/false));
  EXPECT_TRUE(thumbcache::deleteCorruptThumb(/*exists=*/true, /*opened=*/true, /*validBmp=*/false));
}

TEST(ThumbCachePolicy, MissingFileIsNotKept) {
  EXPECT_FALSE(thumbcache::keepExistingThumb(/*exists=*/false, /*opened=*/false, /*validBmp=*/false));
}

TEST(ThumbCachePolicy, AnyCachedThumbSkipsJpeg) {
  using thumbcache::DiskThumb;
  EXPECT_EQ(thumbcache::classify(true, false), DiskThumb::Hero);
  EXPECT_EQ(thumbcache::classify(true, true), DiskThumb::Hero);
  EXPECT_EQ(thumbcache::classify(false, true), DiskThumb::Fallback);
  EXPECT_EQ(thumbcache::classify(false, false), DiskThumb::Missing);
  EXPECT_TRUE(thumbcache::skipJpeg(DiskThumb::Hero));
  EXPECT_TRUE(thumbcache::skipJpeg(DiskThumb::Fallback));
  EXPECT_FALSE(thumbcache::skipJpeg(DiskThumb::Missing));
  EXPECT_FALSE(thumbcache::jpegWhenIdle(DiskThumb::Hero));
  EXPECT_FALSE(thumbcache::jpegWhenIdle(DiskThumb::Fallback));
  EXPECT_TRUE(thumbcache::jpegWhenIdle(DiskThumb::Missing));
}
