#pragma once

// Penumbra Now Reading / Title under-panel: title → author air after the last
// wrapped title line. X4 used a 5px pair gap and 12pt author so the stack
// cleared the hairline; that glued the name to the title and made it look
// smaller than X3 (18px / 14pt). Equal-gap G is the hairline clearance now, so
// both devices share the X3 pair.
namespace penumbra {

constexpr int kTitleToAuthorGap = 18;

inline int titleToAuthorGap() { return kTitleToAuthorGap; }

// Last wrapped line uses ink, not the oversized SS4 advanceY box, so layout G
// sits on the author ink (same as hairline → Recents).
inline int wrappedInkHeight(const int lineCount, const int lineH, const int inkH) {
  if (lineCount <= 0) return 0;
  return (lineCount - 1) * lineH + inkH;
}

// X4 home: five matching air gaps G.
//   status → Now Reading, author → hairline, hairline → Recents,
//   Recents → first book, last book → menu.
// Recents used to use a fixed 12px caption-to-list gap inside Lh while G was
// ~4× larger, so the caption sat on the first row.
struct X4HomeGaps {
  int G = 0;
  int upperTop = 0;
  int midY = 0;
  int recentsTop = 0;
  int firstBookTop = 0;
};

inline X4HomeGaps x4HomeGaps(const int contentTop, const int contentH, const int nowReadingH, const int recentsCaptionH,
                             const int recentsListH, const int ruleH) {
  const int raw = contentH - nowReadingH - recentsCaptionH - recentsListH - ruleH;
  const int free = raw > 0 ? raw : 0;
  X4HomeGaps o;
  o.G = free / 5;
  o.upperTop = contentTop + o.G;
  o.midY = o.upperTop + nowReadingH + o.G;
  o.recentsTop = o.midY + ruleH + o.G;
  o.firstBookTop = o.recentsTop + recentsCaptionH + o.G;
  return o;
}

}  // namespace penumbra
