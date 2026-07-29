#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  bool focusReading = false;  // Bionic Reading
  bool guideReading = false;  // Guide Dots
  bool hyphenation = false;
  bool operator==(const PreviewKey&) const = default;
};

// Cached engine preview lines + the key that produced them.
// Two paragraphs so Extra Paragraph Spacing can show gap vs first-line indent.
struct PreviewLayout {
  std::vector<std::shared_ptr<TextBlock>> para1;
  std::vector<std::shared_ptr<TextBlock>> para2;
  PreviewKey key;
};

// Preview chrome: double-line header (like Settings tabs) with bold "Preview" plus
// font/size, then an unboxed body that lays out sample text at full reader width
// (screen width − 2× screen margin) so line length matches the book page.
// notInPreviewNote: optional note at top of the body (e.g. STR_NOT_IN_PREVIEW).
void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int top, int height, const char* familyName,
                   const char* sizeName, const char* notInPreviewNote = nullptr);

}  // namespace textsettings
