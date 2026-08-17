#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// six parallel vectors: a resident page holds ~25-30 of these blocks, and the
// vector-of-string layout cost ~250 throwing allocations per page load, which
// was the primary driver of heap fragmentation on the ESP32-C3.
//
// Arena layout, in order (2-byte alignment holds by construction: all 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]          byte offset of word i's text in text[]
//   int16_t  xpos[wordCount]
//   uint16_t focusSuffixX[wordCount]     present only when focusPresent
//   uint16_t guideDotXOffset[wordCount]  present only when guideDotsPresent
//   uint8_t  styles[wordCount]
//   uint8_t  focusBoundary[wordCount]    present only when focusPresent
//   char     text[textBytes]             all words back to back, NUL-terminated
//
// Focus split: boundary N > 0 means first N bytes of word i render bold.
// Guide Dots: guideDotXOffset[i] > 0 means draw · at xpos[i] + offset (legacy).
class TextBlock final : public Block {
 private:
  BlockStyle blockStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including NULs
  bool focusPresent = false;
  bool guideDotsPresent = false;
  bool isValid = true;
  std::unique_ptr<uint8_t[]> arena;
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint16_t* focusSuffixXArr = nullptr;     // null when !focusPresent
  const uint16_t* guideDotXOffsetArr = nullptr;  // null when !guideDotsPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const char* textArr = nullptr;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, bool hasGuideDots, uint16_t textBytes);
  void bindArenaPointers();

 public:
  // Flatten-on-construct. Empty focusBoundary / guideDotXOffset omit those arenas.
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const std::vector<uint16_t>& guideDotXOffset,
                     const BlockStyle& blockStyle = BlockStyle());
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  uint8_t focusBoundary(const uint16_t i) const { return focusPresent ? focusBoundaryArr[i] : 0; }
  uint16_t focusSuffixX(const uint16_t i) const { return focusPresent ? focusSuffixXArr[i] : 0; }
  uint16_t guideDotXOffset(const uint16_t i) const { return guideDotsPresent ? guideDotXOffsetArr[i] : 0; }

  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};
