#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

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
  for (uint8_t i = 0; i < kMaxScanBuckets; i++) {
    scanBuckets_[i].fontId = -1;
    scanBuckets_[i].style = 0;
    // clear(), not shrink: the buckets are reused every page, so keeping the
    // capacity avoids re-growing the same strings on every single turn.
    scanBuckets_[i].text.clear();
  }
  scanBucketCount_ = 0;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  ScanBucket* bucket = nullptr;
  for (uint8_t i = 0; i < scanBucketCount_; i++) {
    if (scanBuckets_[i].fontId == fontId && scanBuckets_[i].style == baseStyle) {
      bucket = &scanBuckets_[i];
      break;
    }
  }
  if (!bucket) {
    if (scanBucketCount_ < kMaxScanBuckets) {
      bucket = &scanBuckets_[scanBucketCount_++];
      bucket->fontId = fontId;
      bucket->style = baseStyle;
      bucket->text.clear();
      if (bucket->text.capacity() < 256) bucket->text.reserve(256);
    } else {
      // Out of buckets (a page mixing more than 8 face/style combinations).
      // Fold into a bucket for the same font id when there is one, else the
      // first bucket. That over-prewarms — exactly what the old single-bucket
      // code always did — but never drops glyphs, so rendering stays correct.
      for (uint8_t i = 0; i < scanBucketCount_; i++) {
        if (scanBuckets_[i].fontId == fontId) {
          bucket = &scanBuckets_[i];
          break;
        }
      }
      if (!bucket) bucket = &scanBuckets_[0];
    }
  }
  bucket->text += text;
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
    if (bucket.fontId < 0 || bucket.text.empty()) continue;
    manager_->prewarmCache(bucket.fontId, bucket.text.c_str(), static_cast<uint8_t>(1u << bucket.style));
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
