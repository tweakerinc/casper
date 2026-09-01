#pragma once

// Greyscale refresh (cover AA, clock AA, text AA) is light-polarity: it pushes
// the BW framebuffer and grey planes without invertOnDisplay. That is why Bare
// home covers and "turn on AA" washed Dark Mode back to a white plate.
namespace darkmode {

inline bool systemWide(const bool darkOn, const bool readerOnly) { return darkOn && !readerOnly; }

// Home covers / clock AA / settings preview — skip when invert-on-display is armed.
inline bool skipUiGrayscale(const bool darkOn, const bool readerOnly) { return systemWide(darkOn, readerOnly); }

// Reader page AA — skip for any Dark Mode (reader-only still inverts the page).
inline bool skipReaderGrayscale(const bool darkOn) { return darkOn; }

// Text Anti-Aliasing is a no-op in Dark Mode; hide the toggle so turning it on
// cannot run a light-polarity grey pass over a dark UI.
inline bool showAntiAliasingSetting(const bool darkOn) { return !darkOn; }

}  // namespace darkmode
