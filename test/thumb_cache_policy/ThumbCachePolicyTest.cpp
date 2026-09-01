#include <gtest/gtest.h>

#include "ThumbCachePolicy.h"

TEST(ThumbCachePolicy, KeepWhenProbeCannotOpen) {
  EXPECT_TRUE(thumbcache::keepExistingThumb(/*exists=*/true, /*opened=*/false, /*validBmp=*/false));
  EXPECT_FALSE(thumbcache::deleteCorruptThumb(/*exists=*/true, /*opened=*/false, /*validBmp=*/false));
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

TEST(ThumbCachePolicy, OnlyHeroSizeIsReadyToPaint) {
  EXPECT_TRUE(thumbcache::heroReadyToPaint(/*heroExists=*/true));
  EXPECT_FALSE(thumbcache::heroReadyToPaint(/*heroExists=*/false));
}
