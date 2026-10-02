#include <gtest/gtest.h>

#include "GlyphWeightPolicy.h"

TEST(GlyphWeightPolicy, X3ChromeIsMild) { EXPECT_EQ(glyphweight::chrome(false), glyphweight::Bw::Mild); }

TEST(GlyphWeightPolicy, X4ChromeIsDense) { EXPECT_EQ(glyphweight::chrome(true), glyphweight::Bw::Dense); }

TEST(GlyphWeightPolicy, ReaderAaOffIsMild) { EXPECT_EQ(glyphweight::reader(false), glyphweight::Bw::Mild); }

TEST(GlyphWeightPolicy, ReaderAaOnIsNormal) { EXPECT_EQ(glyphweight::reader(true), glyphweight::Bw::Normal); }

TEST(GlyphWeightPolicy, ValuesMatchRendererEnum) {
  EXPECT_EQ(static_cast<uint8_t>(glyphweight::Bw::Normal), 0);
  EXPECT_EQ(static_cast<uint8_t>(glyphweight::Bw::Mild), 1);
  EXPECT_EQ(static_cast<uint8_t>(glyphweight::Bw::Dense), 2);
}
