#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern.
  // clearOnEnter: free previous page glyphs before scanning (required when filling slots).
  // clearOnExit: free after paint (old default). Set false to retain glyphs for next turn /
  // idle prewarm so the next page can skip a cold decompress.
  class PrewarmScope {
   public:
    PrewarmScope(FontCacheManager& manager, bool clearOnEnter, bool clearOnExit);
    ~PrewarmScope();
    // Returns true if shouldAbort fired before every bucket was cached.
    bool endScanAndPrewarm(bool (*shouldAbort)() = nullptr);
    // Keep page glyph buffers after destroy (idle prewarm / heap-ok page turns).
    void keepCacheOnExit() { clearOnExit_ = false; }
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
    bool clearOnExit_ = true;
  };
  PrewarmScope createPrewarmScope(bool clearOnEnter = true, bool clearOnExit = true);

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // Scan-pass text, bucketed by (fontId, base style).
  //
  // This used to be ONE string plus a single latched `scanFontId_`, which broke
  // in two ways:
  //
  //   1. Only the FIRST font id seen was ever prewarmed. Under Rivulet every
  //      size step resolves to its own font id (FontLadder::resolve) and
  //      PageLayouter forces Heading1=+2 / Heading2=+1, so any page with a
  //      heading or drop cap left those glyphs entirely uncached — every one of
  //      them then fell through to FontDecompressor's hot-group path, which
  //      inflates a whole group per glyph run.
  //   2. The full page text was prewarmed once PER STYLE, so a single italic
  //      word bought a second complete page-glyph buffer plus all of its group
  //      inflates (2-4x the glyph RAM and decompression for a mixed page).
  //
  // Bucketing by (fontId, style) prewarms each face with only the text actually
  // drawn in it. Bucket count is fixed so the scan pass never allocates a
  // container; overflow folds into an existing bucket, which merely restores the
  // old over-prewarm behaviour instead of dropping glyphs.
  static constexpr uint8_t kMaxScanBuckets = 8;
  struct ScanBucket {
    int fontId = -1;
    uint8_t style = 0;  // base style bits (0-3): regular / bold / italic / bold-italic
    std::string text;
  };
  ScanBucket scanBuckets_[kMaxScanBuckets];
  uint8_t scanBucketCount_ = 0;

  void resetScanBuckets();
};
