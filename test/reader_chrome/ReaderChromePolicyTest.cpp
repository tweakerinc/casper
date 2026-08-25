#include <gtest/gtest.h>

#include <cstdlib>

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

TEST(DictLookupLayout, WordHighlightCentersOnInkAtSmallSize) {
  // Size ~10: ascender 10, descender -3. Wide row pitch must not pull the bar down.
  const int wordY = 100;
  const int asc = 10;
  const int desc = -3;
  const auto box = dictlookup::wordHighlightRect(40, wordY, 48, /*linePitch=*/16, asc, desc, /*nextRowY=*/140);
  const int inkH = asc + 3;
  const int inkMid = wordY + inkH / 2;
  const int boxMid = box.y + box.h / 2;
  EXPECT_LE(box.y, wordY);
  EXPECT_GE(box.y + box.h, wordY + inkH);
  EXPECT_LE(std::abs(boxMid - inkMid), 1);
  EXPECT_LT(box.y + box.h, 140);
  EXPECT_EQ(box.y - wordY, (wordY + inkH) - (box.y + box.h));
}

TEST(DictLookupLayout, WordHighlightCentersOnInkAtLargeSize) {
  const int wordY = 80;
  const int asc = 18;
  const int desc = -5;
  const auto box = dictlookup::wordHighlightRect(12, wordY, 60, /*linePitch=*/24, asc, desc, /*nextRowY=*/130);
  const int inkH = asc + 5;
  const int inkMid = wordY + inkH / 2;
  const int boxMid = box.y + box.h / 2;
  EXPECT_LE(box.y, wordY);
  EXPECT_GE(box.y + box.h, wordY + inkH);
  EXPECT_LE(std::abs(boxMid - inkMid), 1);
  EXPECT_EQ(box.y - wordY, (wordY + inkH) - (box.y + box.h));
  EXPECT_GT(box.h, 18);
}

TEST(DictLookupLayout, WordHighlightDoesNotUseRowPitchAsHeight) {
  const auto box = dictlookup::wordHighlightRect(0, 50, 20, /*linePitch=*/40, /*asc=*/12, /*desc=*/-3, /*nextRowY=*/90);
  EXPECT_LT(box.h, 40);
  EXPECT_LT(box.y + box.h, 90);
}

TEST(DictLookupLayout, WordHighlightStaysAboveNextRowWhenTight) {
  const int wordY = 200;
  const int asc = 16;
  const int desc = -4;
  const int inkH = asc + 4;
  const int nextRowY = wordY + inkH + 1;
  const auto box = dictlookup::wordHighlightRect(8, wordY, 30, inkH, asc, desc, nextRowY);
  EXPECT_LT(box.y + box.h, nextRowY);
  EXPECT_GE(box.y + box.h, wordY + inkH);
  EXPECT_EQ(box.y - wordY, (wordY + inkH) - (box.y + box.h));
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
