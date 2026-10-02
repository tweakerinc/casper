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

  // Optional prewarm requests have a fixed memory budget. A full bucket or
  // an excess face is handled on demand during paint, never assigned to the
  // wrong face. Font IDs are signed hashes; negative values are valid.
  static constexpr uint8_t kMaxScanBuckets = 8;
  static constexpr uint16_t kMaxScanGlyphs = 128;
  struct ScanBucket {
    int fontId = 0;
    uint8_t style = 0;
    uint16_t count = 0;
    uint32_t codepoints[kMaxScanGlyphs] = {};
  };
  ScanBucket scanBuckets_[kMaxScanBuckets];
  uint8_t scanBucketCount_ = 0;

  void resetScanBuckets();
};
