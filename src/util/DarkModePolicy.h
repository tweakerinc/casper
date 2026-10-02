#pragma once

// Greyscale refresh (clock AA, settings preview) is light-polarity: it pushes
// the BW framebuffer and grey planes without invertOnDisplay. That is why
// "turn on AA" washed Dark Mode back to a white plate.
//
// Jacket greys are different: invert-on-display wraps the BW base, invertRect
// keeps the photograph original, and grey planes only ink the cover rect.
namespace darkmode {

inline bool systemWide(const bool darkOn, const bool readerOnly) { return darkOn && !readerOnly; }

// Clock AA / settings preview — skip when invert-on-display is armed.
inline bool skipUiGrayscale(const bool darkOn, const bool readerOnly) { return systemWide(darkOn, readerOnly); }

// Home jacket 2-bit pass. Do not skip: Dark Mode covers were 1-bit crushed.
inline bool skipCoverGrayscale(const bool /*darkOn*/, const bool /*readerOnly*/) { return false; }

// Jacket art is a photograph. Whole-UI invert would make it a negative.
inline bool preserveCoverPolarity(const bool darkOn, const bool readerOnly) { return systemWide(darkOn, readerOnly); }

// Opening/Loading/Saving: FAST window cannot drive white glyphs onto a dark plate.
inline bool statusCueNeedsHalf(const bool darkOn, const bool readerOnly) { return systemWide(darkOn, readerOnly); }

// Home/reader menu over a cover or page: FAST leaves the jacket visible.
inline bool menuOpenNeedsHalf(const bool darkOn, const bool readerOnly) { return systemWide(darkOn, readerOnly); }

// Reader page AA — skip for any Dark Mode (reader-only still inverts the page).
inline bool skipReaderGrayscale(const bool darkOn) { return darkOn; }

// Text Anti-Aliasing is a no-op in Dark Mode; hide the toggle so turning it on
// cannot run a light-polarity grey pass over a dark UI.
inline bool showAntiAliasingSetting(const bool darkOn) { return !darkOn; }

}  // namespace darkmode
