#include <gtest/gtest.h>

#include "KeepWithNextPolicy.h"

using rivulet::headingClusterMovesToNextPage;
using rivulet::kKeepWithNextBodyLines;

TEST(KeepWithNextPolicy, StaysWhenTwoBodyLinesFit) {
  const int body = 16;
  const int page = 480;
  const int cluster = 40;
  EXPECT_FALSE(headingClusterMovesToNextPage(false, cluster, 2 * body, body, page));
}

TEST(KeepWithNextPolicy, MovesWhenBodyWouldStartNextPage) {
  const int body = 16;
  const int page = 480;
  const int cluster = 40;
  EXPECT_TRUE(headingClusterMovesToNextPage(false, cluster, body - 1, body, page));
}

TEST(KeepWithNextPolicy, MovesWhenOnlyOneLineFitsAndTwoWouldFitOnAFullPage) {
  const int body = 16;
  const int page = 480;
  const int cluster = 40;
  EXPECT_TRUE(headingClusterMovesToNextPage(false, cluster, body, body, page));
}

TEST(KeepWithNextPolicy, NeverMovesClusterThatStartedAtPageTop) {
  const int body = 16;
  const int page = 480;
  EXPECT_FALSE(headingClusterMovesToNextPage(true, 40, 0, body, page));
}

TEST(KeepWithNextPolicy, TallHeadingFallsBackToOneBodyLine) {
  const int body = 16;
  const int page = 80;
  const int cluster = page - body;  // heading + two lines would overflow a full page
  EXPECT_FALSE(headingClusterMovesToNextPage(false, cluster, body, body, page));
  EXPECT_TRUE(headingClusterMovesToNextPage(false, cluster, body - 1, body, page));
}

TEST(KeepWithNextPolicy, HeadingTallerThanPageDoesNotMove) {
  const int body = 16;
  const int page = 80;
  EXPECT_FALSE(headingClusterMovesToNextPage(false, page + body, 0, body, page));
}

TEST(KeepWithNextPolicy, RejectsInvalidMetrics) {
  EXPECT_FALSE(headingClusterMovesToNextPage(false, 40, 0, 0, 480));
  EXPECT_FALSE(headingClusterMovesToNextPage(false, 40, 0, 16, 0));
}

TEST(KeepWithNextPolicy, DefaultIsTwoBodyLines) { EXPECT_EQ(kKeepWithNextBodyLines, 2); }
