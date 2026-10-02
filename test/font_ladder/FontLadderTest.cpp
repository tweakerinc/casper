// Host tests for FontLadder::resolve.
//
// SD-card families are not in the builtin Literata/Source Serif tables.
// resolve() must return the incoming id at every SizeStep so an SD pack is
// not silently swapped for a builtin face.

#include <gtest/gtest.h>

#include "FontLadder.h"
#include "IrFormat.h"

using rivulet::FontLadder;
using rivulet::SizeStep;

TEST(FontLadder, UnknownFamilyKeepsIdAtEveryStep) {
  constexpr int kSdId = 0x13579bdf;  // not Literata / Source Serif
  EXPECT_EQ(FontLadder::resolve(kSdId, SizeStep::Minus2), kSdId);
  EXPECT_EQ(FontLadder::resolve(kSdId, SizeStep::Minus1), kSdId);
  EXPECT_EQ(FontLadder::resolve(kSdId, SizeStep::Body), kSdId);
  EXPECT_EQ(FontLadder::resolve(kSdId, SizeStep::Plus1), kSdId);
  EXPECT_EQ(FontLadder::resolve(kSdId, SizeStep::Plus2), kSdId);
}

TEST(FontLadder, LiterataBodyStaysOnLadder) {
  constexpr int kLiterata12 = 2090520927;
  constexpr int kLiterata14 = -847079762;
  EXPECT_EQ(FontLadder::resolve(kLiterata12, SizeStep::Body), kLiterata12);
  EXPECT_EQ(FontLadder::resolve(kLiterata12, SizeStep::Plus1), kLiterata14);
}
