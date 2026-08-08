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

// Geometry from the last BW preview paint — enough to re-draw sample text for AA greys.
struct PreviewPaint {
  int fontId = 0;
  int textLeft = 0;
  int sampleTop = 0;
  int bodyBottom = 0;
  int lineH = 0;
  int lineAdvance = 0;
  int paragraphGap = 0;
  bool hasSample = false;
};

// Preview chrome: double-line header with bold "Preview" + font/size, then unboxed
// body at full reader width (screen − 2× margin). Returns paint info for optional
// greyscale multipass of the sample text (Text AA / Font Darkness).
PreviewPaint renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int top, int height,
                           const char* familyName, const char* sizeName);

// Re-draw only the sample body (no chrome). Used for GRAYSCALE_LSB / MSB passes.
void renderPreviewSampleText(const GfxRenderer& renderer, const PreviewLayout& layout, const PreviewPaint& paint);

}  // namespace textsettings
