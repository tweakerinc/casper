#include <gtest/gtest.h>

#include "DropCapPolicy.h"

TEST(DropCapPolicy, HeadingThenDropCapStaysOnSamePage) {
  EXPECT_FALSE(rivulet::dropCapStartsNewPage(/*atBlockStart=*/true, /*yPastOrigin=*/true, /*pageHasProse=*/false));
}

TEST(DropCapPolicy, DropCapAtTopOfPageStays) {
  EXPECT_FALSE(rivulet::dropCapStartsNewPage(true, /*yPastOrigin=*/false, /*pageHasProse=*/false));
}

TEST(DropCapPolicy, DropCapAfterProseStartsNewPage) {
  EXPECT_TRUE(rivulet::dropCapStartsNewPage(true, true, /*pageHasProse=*/true));
}

TEST(DropCapPolicy, MidBlockDoesNotForceBreak) {
  EXPECT_FALSE(rivulet::dropCapStartsNewPage(/*atBlockStart=*/false, true, true));
}
