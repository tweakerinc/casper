#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;   // Bionic Reading
  bool guideReadingEnabled;   // Guide Dots (· between words)
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<bool> reorderedFocusSuffixScratch;
  std::vector<uint16_t> visualOrderScratch;

  // When floatLayoutLineH_ > 0, zones narrow lines that overlap the exclusion box.
  int16_t floatLayoutStartY_ = 0;
  int floatLayoutLineH_ = 0;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // Available text width for lineIndex (0-based). Fast path: returns pageWidth when no zones.
  int widthForLine(int lineIndex, int pageWidth) const;
  // Horizontal shift for a left float on this line (0 if none / right float).
  int leftFloatShiftForLine(int lineIndex) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  // Greedy breaks with per-line width (float zones). Used when floatLayoutLineH_ > 0.
  std::vector<size_t> computeFloatAwareLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const bool guideReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // First token for layout heuristics (epigraph attributions). Empty if none.
  const std::string& firstWord() const {
    static const std::string kEmpty;
    return words.empty() ? kEmpty : words[0];
  }
  // Rivulet drop-cap: peel first non-soft-hyphen UTF-8 codepoint from the first word.
  // Remainder stays as first word with wordContinues so no extra space before the rest.
  // Returns empty string if nothing to peel.
  std::string peelDropCapLetter();
  // maxLines: 0 = all lines; >0 extracts at most that many.
  // blockStartY + lineHeight enable float-zone narrowing (0 lineHeight = fast path, ignore zones).
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, int maxLines = 0, int16_t blockStartY = 0,
                             int lineHeight = 0);
};
