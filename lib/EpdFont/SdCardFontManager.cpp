#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstdlib>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

int SdCardFontManager::loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer) {
  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", file.path.c_str());
    return 0;
  }

  if (!font->load(file.path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", file.path.c_str());
    delete font;
    return 0;
  }

  int fontId = computeFontId(font->contentHash(), familyName, file.pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, file.path.c_str());
    delete font;
    return 0;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, file.pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", file.path.c_str(), file.pointSize, fontId, font->styleCount());

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);
  return fontId;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Select the physical point size closest to the built-in reader sizes. Some
  // CJK font packs only ship larger sizes, so ordinal selection can make
  // MEDIUM load 18pt+ and produce oversized pages on small devices.
  const SdCardFontFileInfo* selected = family.findClosestReaderSize(fontSizeEnum);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  if (loadFile(*selected, family.name.c_str(), renderer) == 0) {
    return false;
  }

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  return true;
}

int SdCardFontManager::loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                           uint8_t pointSize) {
  const SdCardFontFileInfo* file = family.findFile(pointSize);
  if (!file) return 0;  // family has no .cpfont at this exact size

  // Reuse an already-loaded font of the same size (e.g. when a reader size
  // happens to match a UI size) instead of double-loading the file.
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }

  return loadFile(*file, family.name.c_str(), renderer);
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // Drop UI CJK fallbacks before the SD fonts they point at are freed.
  renderer.clearFallbackFonts();
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}

bool SdCardFontManager::ownsFontId(const int fontId) const {
  if (fontId == 0) return false;
  for (const auto& lf : loaded_) {
    if (lf.fontId == fontId) return true;
  }
  return false;
}

bool SdCardFontManager::fillRelativeLadder(const int baseFontId, const SdCardFontFamilyInfo& family,
                                           GfxRenderer& renderer, int outFontIdByStep[5]) {
  if (!ownsFontId(baseFontId) || !outFontIdByStep) return false;

  // Standard reader point sizes (matches builtin Literata/SS4 ladders).
  static constexpr uint8_t kReaderPts[] = {8, 10, 12, 14, 16, 18};
  static constexpr int kN = 6;

  uint8_t basePt = 0;
  for (const auto& lf : loaded_) {
    if (lf.fontId == baseFontId) {
      basePt = lf.size;
      break;
    }
  }
  if (basePt == 0) basePt = loadedPointSize_;
  if (basePt == 0) return false;

  // Map basePt onto the nearest index in kReaderPts for relative step math.
  int baseIdx = 0;
  int bestDist = 255;
  for (int i = 0; i < kN; ++i) {
    const int d = std::abs(static_cast<int>(kReaderPts[i]) - static_cast<int>(basePt));
    if (d < bestDist) {
      bestDist = d;
      baseIdx = i;
    }
  }

  int distinct = 0;
  int lastId = 0;
  // StyleResolve sizeStep is still 0..4 relative to user base (SIZE_STEP_BASE=2).
  // Map those five relative steps onto the absolute 6-rung point ladder.
  static constexpr int kStepBase = 2;
  static constexpr int kStepMax = 4;
  for (int step = 0; step <= kStepMax; ++step) {
    const int absIdx = std::max(0, std::min(kN - 1, baseIdx + (step - kStepBase)));
    const uint8_t wantPt = kReaderPts[absIdx];
    int id = 0;
    // Prefer already-loaded face at this point size.
    for (const auto& lf : loaded_) {
      if (lf.size == wantPt) {
        id = lf.fontId;
        break;
      }
    }
    if (id == 0) {
      id = loadFamilyExtraSize(family, renderer, wantPt);
    }
    // Fallback: closest loaded size if this pt is missing on disk.
    if (id == 0) {
      int best = -1;
      int bestD = 255;
      for (const auto& lf : loaded_) {
        const int d = std::abs(static_cast<int>(lf.size) - static_cast<int>(wantPt));
        if (d < bestD) {
          bestD = d;
          best = lf.fontId;
        }
      }
      id = best > 0 ? best : baseFontId;
    }
    outFontIdByStep[step] = id;
    if (id != 0 && id != lastId) {
      if (lastId != 0) ++distinct;
      lastId = id;
    }
  }
  // Ensure base step maps to the caller's baseFontId when possible.
  outFontIdByStep[kStepBase] = baseFontId;
  // Multi-size only when we actually have more than one face.
  for (int step = 0; step <= kStepMax; ++step) {
    if (outFontIdByStep[step] != 0 && outFontIdByStep[step] != baseFontId) {
      return true;
    }
  }
  return false;
}
