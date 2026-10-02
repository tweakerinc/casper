#include <gtest/gtest.h>

#include "CoverRenderPolicy.h"

using thumbcache::DiskThumb;

TEST(CoverRenderPolicy, ReturnPathStillSkipsJpegWhenAnyThumbExists) {
  EXPECT_TRUE(coverrender::skipJpegOnReturn(DiskThumb::Hero));
  EXPECT_TRUE(coverrender::skipJpegOnReturn(DiskThumb::Fallback));
  EXPECT_TRUE(coverrender::skipJpegOnReturn(DiskThumb::Unverified));
  EXPECT_FALSE(coverrender::skipJpegOnReturn(DiskThumb::Missing));
}

TEST(CoverRenderPolicy, IdleGeneratesMissingAndUpgradesFallback) {
  EXPECT_TRUE(coverrender::generateHero(DiskThumb::Missing));
  EXPECT_TRUE(coverrender::generateHero(DiskThumb::Fallback));
  EXPECT_FALSE(coverrender::generateHero(DiskThumb::Hero));
  EXPECT_FALSE(coverrender::generateHero(DiskThumb::Unverified));
}

TEST(CoverRenderPolicy, CueHitsPanelOnceHomeIsVisible) {
  EXPECT_TRUE(coverrender::showRenderingCoverCue(/*willGenerate=*/true, /*homeUiVisible=*/true));
  EXPECT_FALSE(coverrender::showRenderingCoverCue(/*willGenerate=*/true, /*homeUiVisible=*/false));
  EXPECT_FALSE(coverrender::showRenderingCoverCue(/*willGenerate=*/false, /*homeUiVisible=*/true));
  EXPECT_TRUE(coverrender::cueHitsPanel());
  EXPECT_TRUE(coverrender::genWaitsForHomeShell());
  EXPECT_TRUE(coverrender::cueUsesWindowedRefresh());
}

TEST(CoverRenderPolicy, RetrySurvivesEmptyShellGreys) {
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/1, /*missingHero=*/true, /*greysOnPanel=*/true));
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/7, /*missingHero=*/true, /*greysOnPanel=*/false));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/8, /*missingHero=*/true, /*greysOnPanel=*/true));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/1, /*missingHero=*/false, /*greysOnPanel=*/false));
}

TEST(CoverRenderPolicy, RetryUnverifiedWithoutTruncating) {
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/1, DiskThumb::Unverified, /*greysOnPanel=*/false));
  EXPECT_TRUE(coverrender::keepRetrying(/*attempts=*/7, DiskThumb::Missing, false));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/8, DiskThumb::Unverified, false));
  EXPECT_FALSE(coverrender::keepRetrying(/*attempts=*/1, DiskThumb::Hero, false));
}

TEST(CoverRenderPolicy, MissingCoverDoesNotSettleWhileRetrying) {
  EXPECT_FALSE(coverrender::settleMissingCover(/*attempts=*/1, /*missingHero=*/true));
  EXPECT_FALSE(coverrender::settleMissingCover(7, true));
  EXPECT_TRUE(coverrender::settleMissingCover(8, true));
  EXPECT_TRUE(coverrender::settleMissingCover(1, false));
}

TEST(CoverRenderPolicy, LoanFramebufferAndPaintWhenHeroArrives) {
  EXPECT_TRUE(coverrender::loanFramebufferForDecode());
  EXPECT_TRUE(coverrender::paintWhenHeroArrives());
  EXPECT_TRUE(coverrender::idleUpgradeFallbackToHero());
}

TEST(CoverRenderPolicy, FullProgressiveCoverDecode) {
  EXPECT_TRUE(coverrender::fullProgressiveCoverDecode());
}

TEST(CoverRenderPolicy, BareDefersGreysOnlyWhenArtExists) {
  EXPECT_TRUE(coverrender::deferBareCoverGreys(DiskThumb::Hero));
  EXPECT_TRUE(coverrender::deferBareCoverGreys(DiskThumb::Fallback));
  EXPECT_FALSE(coverrender::deferBareCoverGreys(DiskThumb::Missing));
  EXPECT_FALSE(coverrender::deferBareCoverGreys(DiskThumb::Unverified));
}
