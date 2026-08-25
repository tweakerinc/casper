#include <gtest/gtest.h>

#include "DictLookupLayout.h"
#include "ReaderChromePolicy.h"

namespace {

readerchrome::BottomIn defaultPortrait() {
  readerchrome::BottomIn in;
  in.oBottom = 3;
  in.screenMargin = 10;
  in.statusBarHeight = 26;  // 19px 8pt lane + thin progress
  in.progressBarHeight = 7;
  in.statusBarVerticalMargin = 19;
  in.clearance = 8;
  in.hintStrip = 34;
  in.x4 = false;
  return in;
}

}  // namespace

TEST(DictLookupLayout, ModeTitleSitsInViewableBand) {
  EXPECT_EQ(dictlookup::modeTitleY(9), 13);
  EXPECT_GT(dictlookup::modeTitleY(9), 9);
  EXPECT_EQ(dictlookup::modeTitleWipeH(13, 18, 36), 34);
}

TEST(DictLookupLayout, DefinitionCardClearsModeTitle) {
  const int top = dictlookup::definitionTopReserve(9, 18);
  EXPECT_GE(top, dictlookup::modeTitleY(9) + 18 + dictlookup::kTitleToCardGap);
}

TEST(ReaderChromePolicy, X3PortraitKeepsStackedMargin) {
  const auto in = defaultPortrait();
  EXPECT_EQ(readerchrome::portraitHintStrip(34, false), 34);
  EXPECT_EQ(readerchrome::marginBottom(in), 3 + 10 + 34);
}

TEST(ReaderChromePolicy, X4PortraitDropsStackedGap) {
  auto in = defaultPortrait();
  in.x4 = true;
  in.hintStrip = readerchrome::portraitHintStrip(34, true);
  EXPECT_EQ(in.hintStrip, 24);
  // max(status 26, hint 24) + oBottom 3 — no screenMargin, no clearance.
  EXPECT_EQ(readerchrome::marginBottom(in), 29);
  EXPECT_LT(readerchrome::marginBottom(in), 3 + 10 + 34);
  EXPECT_GE(3 + 10 + 34 - readerchrome::marginBottom(in), 12);
}

TEST(ReaderChromePolicy, LandscapeLeavesHintStripToTheSide) {
  auto in = defaultPortrait();
  in.x4 = true;
  in.hintStrip = 0;
  EXPECT_EQ(readerchrome::marginBottom(in), 3 + 10 + (26 + 8));
}
