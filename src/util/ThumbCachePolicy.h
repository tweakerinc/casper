#pragma once

// Home cover BMPs live on SD. A failed probe after the reader (SD busy) used
// to look like a corrupt file, delete thumb_c30_560.bmp, and re-decode JPEG.
namespace thumbcache {

// File is on disk. Open failed: keep it (transient). Opened and not a BMP: delete.
inline bool keepExistingThumb(const bool exists, const bool opened, const bool validBmp) {
  if (!exists) return false;
  if (!opened) return true;
  return validBmp;
}

inline bool deleteCorruptThumb(const bool exists, const bool opened, const bool validBmp) {
  return exists && opened && !validBmp;
}

// Paint/bind from the hero size only. A smaller preview is not "ready".
inline bool heroReadyToPaint(const bool heroExists) { return heroExists; }

}  // namespace thumbcache
