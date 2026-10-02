#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>
#include "../Memory/BoundedUtf8.h"

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::resetScanBuckets() {
  for (auto& bucket : scanBuckets_) bucket.count = 0;
  scanBucketCount_ = 0;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || !*text) return;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 3;
  ScanBucket* bucket = nullptr;
  for (uint8_t i = 0; i < scanBucketCount_; ++i) {
    if (scanBuckets_[i].fontId == fontId && scanBuckets_[i].style == baseStyle) {
      bucket = &scanBuckets_[i];
      break;
    }
  }
  if (!bucket) {
    if (scanBucketCount_ == kMaxScanBuckets) return;
    bucket = &scanBuckets_[scanBucketCount_++];
    bucket->fontId = fontId;
    bucket->style = baseStyle;
    bucket->count = 0;
  }
  const char* p = text;
  const char* end = p + std::strlen(p);
  while (p < end) {
    uint32_t cp = casper_memory::nextUtf8(p, end);
    if (!cp) break;
    bool found = false;
    for (uint16_t i = 0; i < bucket->count; ++i) {
      if (bucket->codepoints[i] == cp) { found = true; break; }
    }
    if (!found && bucket->count < kMaxScanGlyphs) bucket->codepoints[bucket->count++] = cp;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager, const bool clearOnEnter, const bool clearOnExit)
    : manager_(&manager), clearOnExit_(clearOnExit) {
  manager_->scanMode_ = ScanMode::Scanning;
  if (clearOnEnter) {
    manager_->clearCache();
  }
  manager_->resetStats();
  manager_->resetScanBuckets();
}

bool FontCacheManager::PrewarmScope::endScanAndPrewarm(bool (*shouldAbort)()) {
  manager_->scanMode_ = ScanMode::None;

  // Prewarm each (fontId, style) face with only the text drawn in that face.
  // Buckets are visited in first-seen order, which is reading order, so if the
  // decompressor runs out of page slots the faces carrying the most text on the
  // page are the ones that got cached.
  bool aborted = false;
  for (uint8_t i = 0; i < manager_->scanBucketCount_; i++) {
    if (shouldAbort && shouldAbort()) {
      aborted = true;
      break;
    }
    ScanBucket& bucket = manager_->scanBuckets_[i];
    if (bucket.count == 0) continue;
    char text[kMaxScanGlyphs * 4 + 1];
    char* end = text;
    for (uint16_t j = 0; j < bucket.count; ++j) end = casper_memory::encodeUtf8(end, bucket.codepoints[j]);
    *end = '\0';
    manager_->prewarmCache(bucket.fontId, text, static_cast<uint8_t>(1u << bucket.style));
  }

  manager_->resetScanBuckets();
  return aborted;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scan buckets already reset)
    if (clearOnExit_) {
      manager_->clearCache();
    }
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_), clearOnExit_(other.clearOnExit_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope(const bool clearOnEnter, const bool clearOnExit) {
  return PrewarmScope(*this, clearOnEnter, clearOnExit);
}
