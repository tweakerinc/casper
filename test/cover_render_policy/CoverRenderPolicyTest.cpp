#include <gtest/gtest.h>

#include "CoverRenderPolicy.h"

using thumbcache::DiskThumb;

TEST(CoverRenderPolicy, ReturnPathStillSkipsJpegWhenAnyThumbExists) {
  EXPECT_TRUE(coverrender::skipJpegOnReturn(DiskThumb::Hero));
  EXPECT_TRUE(coverrender::skipJpegOnReturn(DiskThumb::Fallback));
  EXPECT_FALSE(coverrender::skipJpegOnReturn(DiskThumb::Missing));
}

TEST(CoverRenderPolicy, IdleGeneratesMissingAndUpgradesFallback) {
  EXPECT_TRUE(coverrender::generateHero(DiskThumb::Missing));
  EXPECT_TRUE(coverrender::generateHero(DiskThumb::Fallback));
  EXPECT_FALSE(coverrender::generateHero(DiskThumb::Hero));
}

TEST(CoverRenderPolicy, CueHitsPanelOnceHomeIsVisible) {
  EXPECT_TRUE(coverrender::showRenderingCoverCue(/*willGenerate=*/true, /*homeUiVisible=*/true));
  EXPECT_FALSE(coverrender::showRenderingCoverCue(/*willGenerate=*/true, /*homeUiVisible=*/false));
  EXPECT_FALSE(coverrender::showRenderingCoverCue(/*willGenerate=*/false, /*homeUiVisible=*/true));
  EXPECT_TRUE(coverrender::cueHitsPanel());
}

TEST(CoverRenderPolicy, RetrySurvivesEmptyShellGreys) {
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/1, /*missingHero=*/true, /*greysOnPanel=*/true));
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/7, /*missingHero=*/true, /*greysOnPanel=*/false));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/8, /*missingHero=*/true, /*greysOnPanel=*/true));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/1, /*missingHero=*/false, /*greysOnPanel=*/false));
}

TEST(CoverRenderPolicy, LoanFramebufferAndPaintWhenHeroArrives) {
  EXPECT_TRUE(coverrender::loanFramebufferForDecode());
  EXPECT_TRUE(coverrender::paintWhenHeroArrives());
  EXPECT_TRUE(coverrender::idleUpgradeFallbackToHero());
}
