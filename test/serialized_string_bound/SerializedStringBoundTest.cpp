#include <gtest/gtest.h>

#include "SerializedStringBound.h"

TEST(SerializedStringBound, AcceptsShortHrefThatFits) {
  EXPECT_TRUE(serialization::serializedStringFits(24, 100));
  EXPECT_TRUE(serialization::serializedStringFits(0, 0));
  EXPECT_TRUE(serialization::serializedStringFits(serialization::kMaxSerializedStringBytes,
                                                  serialization::kMaxSerializedStringBytes));
}

TEST(SerializedStringBound, RejectsTornLengthThatFitsInALargeFile) {
  // v51 crash: persistHomeProgress raced the render task's book.bin seek.
  // The torn uint32 still fitted in a multi-megabyte cache, then resize aborted.
  constexpr uint32_t torn = 0x000B330C;  // 733964 — from the crash stack dump
  constexpr size_t bookBinRemaining = 2 * 1024 * 1024;
  EXPECT_FALSE(serialization::serializedStringFits(torn, bookBinRemaining));
}

TEST(SerializedStringBound, RejectsLengthPastRemaining) {
  EXPECT_FALSE(serialization::serializedStringFits(16, 15));
  EXPECT_FALSE(serialization::serializedStringFits(1, 0));
}

TEST(SerializedStringBound, RejectsRvpgCrashSizedStringLength) {
  // Device abort stack (2026-08-23) had 0x7980 next to LaidOutPage::loadFromFile.
  // 31104 > kMaxSerializedStringBytes, so tryReadString must refuse before resize.
  EXPECT_FALSE(serialization::serializedStringFits(0x7980, 64 * 1024));
}
