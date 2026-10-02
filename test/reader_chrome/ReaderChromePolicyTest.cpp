#include <gtest/gtest.h>

#include <cstdlib>

#include "DictLookupLayout.h"
#include "ReaderChromePolicy.h"
#include "SystemChromePolicy.h"

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

TEST(DictLookupLayout, WordUnderlineSitsJustUnderEmBox) {
  // Clipping/footnote mark: not a dither rect over the line box.
  EXPECT_EQ(dictlookup::wordUnderlineY(100, 12), 114);
  EXPECT_EQ(dictlookup::wordUnderlineY(100, 4), 108);  // floor ascender at 6
  EXPECT_GT(dictlookup::wordUnderlineY(100, 12), 100 + 12);
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
  EXPECT_EQ(in.hintStrip, 34);
  // Overlay: max(status 26, hint 34) + oBottom 3 — no screenMargin, no clearance.
  EXPECT_EQ(readerchrome::marginBottom(in), 37);
  EXPECT_LT(readerchrome::marginBottom(in), 3 + 10 + 34);
  EXPECT_GE(3 + 10 + 34 - readerchrome::marginBottom(in), 10);
}

TEST(ReaderChromePolicy, X4FooterBandMatchesX3Air) {
  constexpr int kBareFooter = 34;
  constexpr int kUi10Line = 17;
  EXPECT_EQ(readerchrome::portraitHintStrip(kBareFooter, true), kBareFooter);
  EXPECT_EQ(readerchrome::portraitHintStrip(kBareFooter, false), kBareFooter);
  // 24px band: 3px air, eaten by the 3px bezel. 34px: same 8px as X3.
  EXPECT_EQ(readerchrome::portraitFooterLabelAir(24, kUi10Line), 3);
  EXPECT_EQ(readerchrome::portraitFooterLabelAir(kBareFooter, kUi10Line), 8);
  EXPECT_GT(readerchrome::portraitFooterLabelAir(kBareFooter, kUi10Line), 3);
}

TEST(ReaderChromePolicy, X4PortraitFooterLiftsOffPanelEdge) {
  constexpr int kBareFooter = 34;
  constexpr int kX4Pad = readerchrome::kX4PortraitFooterEdgePad;
  EXPECT_EQ(kX4Pad, 8);
  EXPECT_LT(kX4Pad, 34);  // must not steal a Recents row
  EXPECT_EQ(readerchrome::portraitFooterEdgePad(false), 0);
  EXPECT_EQ(readerchrome::portraitFooterEdgePad(true), kX4Pad);
  EXPECT_EQ(readerchrome::portraitFooterLayoutH(kBareFooter, false), kBareFooter);
  EXPECT_EQ(readerchrome::portraitFooterLayoutH(kBareFooter, true), kBareFooter + kX4Pad);
  // Same 34px band Y on both panels: X3 flush on 792, X4's extra 8px is below.
  EXPECT_EQ(readerchrome::portraitFooterBarY(792, kBareFooter, 0, false), 758);
  EXPECT_EQ(readerchrome::portraitFooterBarY(800, kBareFooter, kX4Pad, false), 758);
  EXPECT_EQ(readerchrome::portraitFooterBarY(800, kBareFooter, kX4Pad, true), kX4Pad);
  EXPECT_EQ(readerchrome::portraitFooterBarY(792, kBareFooter, 0, true), 0);
}

TEST(ReaderChromePolicy, X4ReaderOverlayDoesNotIncludeFooterEdgePad) {
  auto in = defaultPortrait();
  in.x4 = true;
  in.hintStrip = 34;
  EXPECT_EQ(readerchrome::marginBottom(in), 37);
  EXPECT_EQ(readerchrome::portraitFooterLayoutH(34, true) - in.hintStrip, readerchrome::kX4PortraitFooterEdgePad);
}

TEST(ReaderChromePolicy, LandscapeLeavesHintStripToTheSide) {
  auto in = defaultPortrait();
  in.x4 = true;
  in.hintStrip = 0;
  EXPECT_EQ(readerchrome::marginBottom(in), 3 + 10 + (26 + 8));
}

TEST(SystemChromePolicy, BareDefaultPaintsWarningWhenLow) {
  systemchrome::HomeTopBarIn in;
  in.hasWarningSlot = true;
  in.warnThresholdPercent = 15;
  in.batteryPercent = 12;
  EXPECT_TRUE(systemchrome::warningIsLive(in));
  EXPECT_TRUE(systemchrome::needsHomeTopBar(in));
}

TEST(SystemChromePolicy, BareDefaultSkipsBarWhenCharged) {
  systemchrome::HomeTopBarIn in;
  in.hasWarningSlot = true;
  in.warnThresholdPercent = 15;
  in.batteryPercent = 80;
  EXPECT_FALSE(systemchrome::warningIsLive(in));
  EXPECT_FALSE(systemchrome::needsHomeTopBar(in));
}

TEST(SystemChromePolicy, BatterySlotStillShowsBarWhenCharged) {
  systemchrome::HomeTopBarIn in;
  in.hasBattery = true;
  in.batteryPercent = 80;
  EXPECT_TRUE(systemchrome::needsHomeTopBar(in));
}

TEST(SystemChromePolicy, ChromeTextClearsPortraitBezelOnBothPanels) {
  constexpr int kViewableTop = 9;
  EXPECT_GT(systemchrome::textY(kViewableTop, 800), kViewableTop);  // X4 portrait
  EXPECT_GT(systemchrome::textY(kViewableTop, 792), kViewableTop);  // X3 portrait
  EXPECT_GE(systemchrome::textY(kViewableTop, 800), 13);
  EXPECT_EQ(systemchrome::textY(5, 800), 5 + systemchrome::textAir(800));  // old Y≈5 was inside the bezel
}

TEST(SystemChromePolicy, WarningUsesFullWidthWhenSidesEmpty) {
  EXPECT_EQ(systemchrome::warningMaxWidth(480, 15, 15, false), 450);  // X4
  EXPECT_EQ(systemchrome::warningMaxWidth(528, 15, 15, false), 498);  // X3
  EXPECT_GT(systemchrome::warningMaxWidth(528, 15, 15, false), systemchrome::warningMaxWidth(480, 15, 15, false));
  EXPECT_LT(systemchrome::warningMaxWidth(480, 15, 15, true), systemchrome::warningMaxWidth(480, 15, 15, false));
}
