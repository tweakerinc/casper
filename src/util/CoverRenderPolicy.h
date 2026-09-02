#pragma once

#include <cstdint>

#include "ThumbCachePolicy.h"

// Home jacket JPEG. Field logs (v0.1.9.0010 / 0014): `thumbs_missing` then
// 60s+ of silence, then `bw_no_path`. Four stacked bugs:
//
// 1. Corner "Loading" used refresh=false while recentsLoading blocked render(),
//    so the cue never reached glass.
// 2. Gen required 90 KB free + 80 KB maxAlloc. After reading, maxAlloc is often
//    30–73 KB even with ~114 KB free — defer forever.
// 3. Empty-shell HALF set coverGrayOnPanel and cancelled retries.
// 4. A leftover 280/168 skipped JPEG on Back (correct) *and* idle (wrong), so
//    some jackets stayed the small dither forever.
//
// Return path still blits any cached thumb (no JPEG hitch). Idle may generate
// the 1:1 560 with a visible "Rendering Cover" cue.
namespace coverrender {

inline constexpr bool loanFramebufferForDecode() { return true; }

inline constexpr bool cueHitsPanel() { return true; }

inline constexpr bool retryAfterShellPaint() { return true; }

inline constexpr bool paintWhenHeroArrives() { return true; }

inline constexpr bool idleUpgradeFallbackToHero() { return true; }

inline constexpr uint8_t kMaxGenAttempts = 8;

inline constexpr unsigned kRetryDelayMs = 800;

inline constexpr bool skipJpegOnReturn(const thumbcache::DiskThumb state) { return thumbcache::skipJpeg(state); }

inline constexpr bool generateHero(const thumbcache::DiskThumb state) {
  if (state == thumbcache::DiskThumb::Missing) return true;
  if (state == thumbcache::DiskThumb::Fallback) return idleUpgradeFallbackToHero();
  return false;
}

inline constexpr bool showRenderingCoverCue(const bool willGenerate, const bool homeUiVisible) {
  return willGenerate && homeUiVisible && cueHitsPanel();
}

inline constexpr bool keepRetrying(const uint8_t attempts, const bool missingHero, const bool /*greysOnPanel*/) {
  if (!missingHero) return false;
  if (attempts >= kMaxGenAttempts) return false;
  return retryAfterShellPaint();
}

}  // namespace coverrender
