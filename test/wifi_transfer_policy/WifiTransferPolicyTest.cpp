#include <gtest/gtest.h>

#include "WifiTransferPolicy.h"

TEST(WifiTransferPolicy, FontManifestUsesLiveCrossinkBucket) {
  EXPECT_STREQ(wifitransfer::kFontManifestHost, "crossink-fonts.s3.us-east-1.amazonaws.com");
  const std::string url = wifitransfer::fontManifestUrl(1, 4);
  EXPECT_EQ(url, "http://crossink-fonts.s3.us-east-1.amazonaws.com/sd-fonts-m1-b4/fonts.json");
  EXPECT_EQ(url.find("legacy-fonts"), std::string::npos);
}

TEST(WifiTransferPolicy, DoNotPaintEinkDuringTls) { EXPECT_FALSE(wifitransfer::paintDuringTlsTransfer()); }

TEST(WifiTransferPolicy, LoanFramebufferForTls) { EXPECT_TRUE(wifitransfer::loanFramebufferForTls()); }

TEST(WifiTransferPolicy, RetryCountsArePositive) {
  EXPECT_GE(wifitransfer::kOtaCheckAttempts, 3);
  EXPECT_GE(wifitransfer::kOtaDownloadAttempts, 3);
  EXPECT_GE(wifitransfer::kFontFileAttempts, 3);
}

TEST(WifiTransferPolicy, MissingAssetIsNoUpdateNotFailed) { EXPECT_TRUE(wifitransfer::noMatchingAssetIsNoUpdate()); }
