#include <gtest/gtest.h>

#include "PenumbraNowReadingPolicy.h"

TEST(PenumbraNowReadingPolicy, X3AndX4ShareTitleAuthorGap) {
  EXPECT_EQ(penumbra::titleToAuthorGap(), 18);
  EXPECT_EQ(penumbra::kTitleToAuthorGap, 18);
}

TEST(PenumbraNowReadingPolicy, GapIsNotTheOldX4FivePx) { EXPECT_GT(penumbra::titleToAuthorGap(), 5); }
