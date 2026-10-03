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
// Bare white plate + "Rendering Cover" (v0.2.0 field):
// 5. onResume set homeUiReady before the shell paint. loop() then generated
//    while recentsLoading blocked render(), and the cue FAST-blitted the empty
//    FB — a white screen with only that word in the corner.
// 6. Header-only / junk thumbs classified as Hero, skipJpeg forever.
// 7. bw_no_path settled coverGrayOnPanel so a later good thumb never redrew.
//
// Return path still blits any cached thumb (no JPEG hitch). Idle may generate
// the 1:1 560 with a visible "Rendering Cover" cue — only after the shell is
// on glass.
namespace coverrender {

inline constexpr bool loanFramebufferForDecode() { return true; }

inline constexpr bool cueHitsPanel() { return true; }

inline constexpr bool retryAfterShellPaint() { return true; }

inline constexpr bool paintWhenHeroArrives() { return true; }

inline constexpr bool idleUpgradeFallbackToHero() { return true; }

// Progressive jackets: jpgd+spill when maxAlloc allows (see CoverDecodePolicy).
// X3 field maxAlloc≈69 KB always NOTENOUGHMEM; JPEGDEC 1/8 is the working path
// and must pass JPEG_SCALE_EIGHTH into decode() (cover path used 0 → err=2).
inline constexpr bool fullProgressiveCoverDecode() { return true; }

// Cue / JPEG run only after HomeActivity::render has put chrome on glass.
inline constexpr bool genWaitsForHomeShell() { return true; }

// Stop a no-progress generation pass from automatically replaying the same
// decoder failure. Re-entering Home or explicitly refreshing starts a new pass.
// Deferral before generation (shell not ready) must still be allowed to retry.
inline constexpr bool retryGenerationPass(const bool fromGeneration, const bool failedWithoutProgress) {
  return !fromGeneration || !failedWithoutProgress;
}

// A missing-art shell may settle once automatic work is exhausted or paused.
// Successful later generation explicitly invalidates the shell before repaint.
inline constexpr bool settleWhenNoCoverWork(const bool recentsLoaded, const bool retryPending) {
  return recentsLoaded && !retryPending;
}

inline constexpr uint8_t kMaxGenAttempts = 8;

inline constexpr unsigned kRetryDelayMs = 800;

inline constexpr bool skipJpegOnReturn(const thumbcache::DiskThumb state) { return thumbcache::skipJpeg(state); }

inline constexpr bool generateHero(const thumbcache::DiskThumb state) {
  if (state == thumbcache::DiskThumb::Hero) return false;
  if (state == thumbcache::DiskThumb::Unverified) return false;  // exists; do not truncate
  if (state == thumbcache::DiskThumb::Fallback) return idleUpgradeFallbackToHero();
  return true;  // Missing
}

inline constexpr bool showRenderingCoverCue(const bool willGenerate, const bool homeUiVisible) {
  return willGenerate && homeUiVisible && cueHitsPanel();
}

inline constexpr bool keepRetrying(const uint8_t attempts, const thumbcache::DiskThumb state,
                                   const bool /*greysOnPanel*/) {
  if (state == thumbcache::DiskThumb::Hero) return false;
  if (attempts >= kMaxGenAttempts) return false;
  if (state == thumbcache::DiskThumb::Fallback) return idleUpgradeFallbackToHero();
  return retryAfterShellPaint();  // Missing or Unverified
}

// Compat for call sites that only know "hero still missing".
inline constexpr bool keepRetrying(const uint8_t attempts, const bool missingHero, const bool greysOnPanel) {
  const auto state = missingHero ? thumbcache::DiskThumb::Missing : thumbcache::DiskThumb::Hero;
  return keepRetrying(attempts, state, greysOnPanel);
}

// Missing-art BW fallback must not claim the panel while JPEG/open retries remain.
inline constexpr bool settleMissingCover(const uint8_t attempts, const bool missingHero) {
  return !keepRetrying(attempts, missingHero, false);
}

inline constexpr bool cueUsesWindowedRefresh() { return true; }

// Bare FAST-then-defer-greys is only for a real jacket. A missing/unverified
// thumb would FAST a white placeholder and then settle via bw_no_path.
inline constexpr bool deferBareCoverGreys(const thumbcache::DiskThumb state) {
  return state == thumbcache::DiskThumb::Hero || state == thumbcache::DiskThumb::Fallback;
}

}  // namespace coverrender
