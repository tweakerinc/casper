#include <gtest/gtest.h>

#include "PenumbraNowReadingPolicy.h"

TEST(PenumbraNowReadingPolicy, X3AndX4ShareTitleAuthorGap) {
  EXPECT_EQ(penumbra::titleToAuthorGap(), 18);
  EXPECT_EQ(penumbra::kTitleToAuthorGap, 18);
}

TEST(PenumbraNowReadingPolicy, GapIsNotTheOldX4FivePx) { EXPECT_GT(penumbra::titleToAuthorGap(), 5); }

TEST(PenumbraNowReadingPolicy, WrappedInkHeightUsesInkOnLastLine) {
  EXPECT_EQ(penumbra::wrappedInkHeight(0, 20, 14), 0);
  EXPECT_EQ(penumbra::wrappedInkHeight(1, 20, 14), 14);
  EXPECT_EQ(penumbra::wrappedInkHeight(2, 20, 14), 34);
}

TEST(PenumbraNowReadingPolicy, X4HairlineAndRecentsShareG) {
  const auto g = penumbra::x4HomeGaps(/*contentTop=*/40, /*contentH=*/700, /*nowReadingH=*/100,
                                      /*recentsCaptionH=*/18, /*recentsListH=*/200, /*ruleH=*/2);
  EXPECT_GT(g.G, 12);
  EXPECT_EQ(g.midY - (g.upperTop + 100), g.G);
  EXPECT_EQ(g.recentsTop - (g.midY + 2), g.G);
  EXPECT_EQ(g.firstBookTop - (g.recentsTop + 18), g.G);
  EXPECT_EQ(g.recentsTop - (g.upperTop + 100), 2 * g.G + 2);
}
