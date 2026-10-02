#pragma once

#include <string>

// WiFi OTA and SD-font download. Sitting transfers must finish; e-ink paint
// during TLS is what made them look "always failed".
//
// Fonts: v0.1.9 pointed FONT_MANIFEST_URL at legacy-fonts (NoSuchBucket, 404).
// The live catalog is still crossink-fonts (HTTP, ~30 KB fonts.json).
//
// OTA: GitHub latest is a ~5.4 MB HTTPS asset. Painting the progress bar
// every 64 KB runs a full panel refresh while wolfSSL holds the socket —
// C3 stalls past the 60 s timeout or OOMs. Loan the 48 KB framebuffer for
// TLS, retry, and paint only between transfers / during flash (no TLS).
namespace wifitransfer {

inline constexpr char kFontManifestHost[] = "crossink-fonts.s3.us-east-1.amazonaws.com";

inline std::string fontManifestUrl(const int manifestVersion, const int cpfontVersion) {
  return std::string("http://") + kFontManifestHost + "/sd-fonts-m" + std::to_string(manifestVersion) + "-b" +
         std::to_string(cpfontVersion) + "/fonts.json";
}

inline constexpr bool paintDuringTlsTransfer() { return false; }

inline constexpr bool loanFramebufferForTls() { return true; }

inline constexpr int kOtaCheckAttempts = 3;
inline constexpr int kOtaDownloadAttempts = 3;
inline constexpr int kFontFileAttempts = 3;
inline constexpr unsigned kRetryDelayMs = 1500;

// checkForUpdate() returned this: map to the "No update" screen, not "Update failed".
inline constexpr bool noMatchingAssetIsNoUpdate() { return true; }

}  // namespace wifitransfer
