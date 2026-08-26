#pragma once

// Penumbra Now Reading / Title under-panel: title → author air after the last
// wrapped title line. X4 used a 5px pair gap and 12pt author so the stack
// cleared the hairline; that glued the name to the title and made it look
// smaller than X3 (18px / 14pt). Equal-gap G is the hairline clearance now, so
// both devices share the X3 pair.
namespace penumbra {

constexpr int kTitleToAuthorGap = 18;

inline int titleToAuthorGap() { return kTitleToAuthorGap; }

}  // namespace penumbra
