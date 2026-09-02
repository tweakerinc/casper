#pragma once

// Casper product identity — firmware on CrossPoint-compatible SD layout.
// Storage root: /.crosspoint only (settings, stats, book package cache).

namespace CasperProduct {

inline constexpr const char* kName = "Casper";
inline constexpr const char* kStorageRoot = "/.crosspoint";

inline constexpr bool kHasKOReader = true;
inline constexpr bool kHasOpds = true;
inline constexpr bool kHasFontDownload = true;
inline constexpr bool kHasBootForeignMigrate = false;
inline constexpr bool kRuntimeDualReadForeign = false;
inline constexpr bool kHasLiterataBuiltin = true;
inline constexpr bool kEnglishOnly = false;

}  // namespace CasperProduct
