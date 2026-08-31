#include "HtmlToIr.h"

#include <Esp.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>
#include <string>

#include "PunctEmphasisPolicy.h"

namespace rivulet {
namespace {

bool ieq(const char* a, size_t an, const char* b) {
  size_t bn = 0;
  while (b[bn]) ++bn;
  if (an != bn) return false;
  for (size_t i = 0; i < an; ++i) {
    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return false;
  }
  return true;
}

bool startsWithI(const char* p, const char* end, const char* lit) {
  while (*lit && p < end) {
    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*lit)));
    if (ca != cb) return false;
    ++p;
    ++lit;
  }
  return *lit == '\0';
}

bool containsI(const char* hay, size_t hayLen, const char* needle) {
  const size_t nlen = std::strlen(needle);
  if (nlen == 0 || hayLen < nlen) return false;
  for (size_t i = 0; i + nlen <= hayLen; ++i) {
    bool ok = true;
    for (size_t j = 0; j < nlen; ++j) {
      const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
      const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
      if (ca != cb) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

// -fno-exceptions: std::string::reserve aborts if new fails. Probe full newCap
// (reserve allocates a whole new buffer, not just the delta).
bool safeReserve(std::string& dst, const size_t needCap) {
  if (dst.capacity() >= needCap) return true;
  if (ESP.getMaxAllocHeap() < needCap + 2048) return false;
  void* p = ::operator new(needCap + 64, std::nothrow);
  if (!p) return false;
  ::operator delete(p);
  dst.reserve(needCap);
  return dst.capacity() >= needCap;
}

bool safePushChar(std::string& dst, const char c) {
  if (dst.size() + 1 > dst.capacity()) {
    size_t nc = dst.capacity() ? dst.capacity() * 2 : 64;
    if (nc < dst.size() + 32) nc = dst.size() + 32;
    if (nc > 8192) nc = std::max(dst.size() + 32, size_t(8192));  // textAcc is flushed often
    if (!safeReserve(dst, nc)) return false;
  }
  dst.push_back(c);
  return true;
}

bool safeAppendLit(std::string& dst, const char* lit) {
  while (*lit) {
    if (!safePushChar(dst, *lit++)) return false;
  }
  return true;
}

// Normalize only what we must; keep real book typography.
//
// This used to flatten curly quotes to ' and ", en/em dashes to -, and the
// ellipsis to "...", on the grounds of avoiding tofu. The builtin faces carry
// all of those (see builtinFonts/literata_*: U+2014, U+2018/19, U+201C/D/E),
// so the mapping bought nothing and cost the thing that makes a page look
// typeset rather than like a plain-text dump — which is precisely the "feels
// cheap, not book-like" complaint. Zero-width and formatting characters are
// still dropped, and no-break spaces still become spaces, because those DO
// break layout rather than merely look different.
bool appendNormalizedUtf8(std::string& dst, uint32_t cp) {
  switch (cp) {
    case 0x00A0:  // nbsp
    case 0x202F:  // narrow nbsp
      return safePushChar(dst, ' ');
    case 0x00AD:  // soft hyphen
    case 0x200B:  // zwsp
    case 0x200C:
    case 0x200D:
    case 0xFEFF:
      return true;  // drop
    default: {
      // utf8AppendCodepoint can grow unchecked — stage then copy with checks.
      std::string tmp;
      tmp.reserve(4);
      utf8AppendCodepoint(cp, tmp);
      for (char ch : tmp) {
        if (!safePushChar(dst, ch)) return false;
      }
      return true;
    }
  }
}

// Decode a handful of entities into utf8 out. Returns bytes consumed after '&'.
// Sets *oom on heap failure (caller should stop convert).
size_t decodeEntity(const char* p, const char* end, std::string& out, bool* oom) {
  if (oom) *oom = false;
  if (p >= end || *p != '&') return 0;
  const char* s = p + 1;
  if (s < end && *s == '#') {
    ++s;
    uint32_t cp = 0;
    if (s < end && (*s == 'x' || *s == 'X')) {
      ++s;
      while (s < end && std::isxdigit(static_cast<unsigned char>(*s))) {
        const char c = *s++;
        cp *= 16;
        if (c >= '0' && c <= '9')
          cp += static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
          cp += static_cast<uint32_t>(c - 'a' + 10);
        else
          cp += static_cast<uint32_t>(c - 'A' + 10);
      }
    } else {
      while (s < end && std::isdigit(static_cast<unsigned char>(*s))) {
        cp = cp * 10 + static_cast<uint32_t>(*s - '0');
        ++s;
      }
    }
    if (s < end && *s == ';') ++s;
    if (cp != 0 && !appendNormalizedUtf8(out, cp)) {
      if (oom) *oom = true;
    }
    return static_cast<size_t>(s - p);
  }
  const char* e = s;
  while (e < end && e - s < 16 && std::isalpha(static_cast<unsigned char>(*e))) ++e;
  const size_t n = static_cast<size_t>(e - s);
  auto match = [&](const char* name, const char* repl) {
    if (ieq(s, n, name)) {
      if (!safeAppendLit(out, repl)) {
        if (oom) *oom = true;
      }
      return true;
    }
    return false;
  };
  // Prefer ASCII punctuation so builtin faces always have the glyphs.
  bool ok = match("amp", "&") || match("lt", "<") || match("gt", ">") || match("quot", "\"") || match("apos", "'") ||
            match("nbsp", " ") || match("mdash", "-") || match("ndash", "-") || match("hellip", "...") ||
            match("lsquo", "'") || match("rsquo", "'") || match("ldquo", "\"") || match("rdquo", "\"");
  if (e < end && *e == ';') ++e;
  if (!ok) {
    if (!safePushChar(out, '&')) {
      if (oom) *oom = true;
    }
    return 1;
  }
  return static_cast<size_t>(e - p);
}

bool appendCollapsedText(std::string& dst, const char* p, size_t n, bool preserveSpace) {
  const char* s = p;
  const char* end = p + n;
  while (s < end) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x80) {
      if (c == '\r') {
        ++s;
        continue;
      }
      if (c == '\n' || c == '\t' || c == ' ') {
        if (!preserveSpace) {
          if (dst.empty() || dst.back() == ' ') {
            ++s;
            continue;
          }
          if (!safePushChar(dst, ' ')) return false;
        } else {
          if (!safePushChar(dst, static_cast<char>(c == '\n' || c == '\t' ? ' ' : c))) return false;
        }
        ++s;
        continue;
      }
      if (!safePushChar(dst, static_cast<char>(c))) return false;
      ++s;
      continue;
    }
    const unsigned char* us = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* before = us;
    const uint32_t cp = utf8NextCodepoint(&us);
    if (cp == 0 || us == before) {
      ++s;
      continue;
    }
    if (!appendNormalizedUtf8(dst, cp)) return false;
    s = reinterpret_cast<const char*>(us);
  }
  return true;
}

struct Tag {
  const char* name = nullptr;
  size_t nameLen = 0;
  bool closing = false;
  bool selfClose = false;
  const char* attr = nullptr;
  const char* attrEnd = nullptr;
};

size_t parseTag(const char* p, const char* end, Tag& tag) {
  if (p >= end || *p != '<') return 0;
  const char* s = p + 1;
  tag = {};
  if (s < end && *s == '/') {
    tag.closing = true;
    ++s;
  }
  if (s < end && (*s == '!' || *s == '?')) {
    while (s < end && *s != '>') ++s;
    if (s < end) ++s;
    return static_cast<size_t>(s - p);
  }
  tag.name = s;
  while (s < end && (std::isalnum(static_cast<unsigned char>(*s)) || *s == ':' || *s == '-')) ++s;
  tag.nameLen = static_cast<size_t>(s - tag.name);
  tag.attr = s;
  while (s < end && *s != '>') {
    if (*s == '/' && s + 1 < end && s[1] == '>') {
      tag.selfClose = true;
      s += 2;
      tag.attrEnd = s - 2;
      return static_cast<size_t>(s - p);
    }
    ++s;
  }
  tag.attrEnd = s;
  if (s < end) ++s;
  return static_cast<size_t>(s - p);
}

// Find attr value for name (class, id, style). Sets *vLen. Returns pointer into tag attrs.
const char* attrValue(const Tag& tag, const char* name, size_t* vLen) {
  *vLen = 0;
  if (!tag.attr || tag.attr >= tag.attrEnd) return nullptr;
  const char* p = tag.attr;
  const char* end = tag.attrEnd;
  const size_t nlen = std::strlen(name);
  while (p + nlen < end) {
    // word-boundary-ish: start or non-alnum before name
    if ((p == tag.attr || !std::isalnum(static_cast<unsigned char>(p[-1]))) && startsWithI(p, end, name)) {
      const char* q = p + nlen;
      while (q < end && (*q == ' ' || *q == '\t')) ++q;
      if (q >= end || *q != '=') {
        ++p;
        continue;
      }
      ++q;
      while (q < end && (*q == ' ' || *q == '\t')) ++q;
      char quote = 0;
      if (q < end && (*q == '"' || *q == '\'')) {
        quote = *q++;
      }
      const char* v0 = q;
      while (q < end && ((quote && *q != quote) || (!quote && *q != ' ' && *q != '\t' && *q != '/'))) ++q;
      *vLen = static_cast<size_t>(q - v0);
      return v0;
    }
    ++p;
  }
  return nullptr;
}

bool attrHasClass(const Tag& tag, const char* needle) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "class", &vlen);
  return v && containsI(v, vlen, needle);
}

bool isHiddenHost(const Tag& tag) {
  // CSS classes that mean "do not show" in many EPUBs (e.g. h1.oculto).
  if (attrHasClass(tag, "oculto") || attrHasClass(tag, "hidden") || attrHasClass(tag, "sr-only") ||
      attrHasClass(tag, "screenreader") || attrHasClass(tag, "screen-reader") || attrHasClass(tag, "visually-hidden") ||
      attrHasClass(tag, "hide") || attrHasClass(tag, "displaynone") || attrHasClass(tag, "display-none")) {
    return true;
  }
  // HTML5 hidden= / hidden="hidden" (EPUB nav landmarks & page-list).
  size_t vlen = 0;
  if (attrValue(tag, "hidden", &vlen) != nullptr) return true;
  // Inline style display:none (Isako TOC quote wrappers, etc.).
  const char* st = attrValue(tag, "style", &vlen);
  if (st && vlen > 0 &&
      (containsI(st, vlen, "display:none") || containsI(st, vlen, "display: none") ||
       containsI(st, vlen, "visibility:hidden") || containsI(st, vlen, "visibility: hidden"))) {
    return true;
  }
  return false;
}

bool attrHasId(const Tag& tag, const char* needle) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "id", &vlen);
  return v && containsI(v, vlen, needle);
}

// True if class/id looks like a *text* title host (not layout wrappers).
// Rejects Calibre/Kobo nests: title-page-*, heading1/2 wrappers, element-number-block —
// those were opening empty centered blocks and blowing chapter-page density.
bool looksLikeTitleHost(const Tag& tag) {
  size_t clen = 0, ilen = 0;
  const char* c = attrValue(tag, "class", &clen);
  const char* id = attrValue(tag, "id", &ilen);
  auto hit = [](const char* v, size_t n) -> bool {
    if (!v || n == 0) return false;
    // Layout shells — never promote to a text block.
    if (containsI(v, n, "title-page") || containsI(v, n, "titlepage") || containsI(v, n, "contributor") ||
        containsI(v, n, "heading-content") || containsI(v, n, "element-number") || containsI(v, n, "heading1") ||
        containsI(v, n, "heading2") || containsI(v, n, "heading3") || containsI(v, n, "heading4") ||
        containsI(v, n, "heading5") || containsI(v, n, "heading6")) {
      return false;
    }
    return containsI(v, n, "chapter-title") || containsI(v, n, "chaptitle") || containsI(v, n, "book-title") ||
           containsI(v, n, "subtitle") || containsI(v, n, "caption") || containsI(v, n, "epigraph") ||
           // bare "chapter" / "title" only when not a compound layout name
           (containsI(v, n, "chapter") && !containsI(v, n, "chapter-body")) || (containsI(v, n, "title") && n < 24);
  };
  return hit(c, clen) || hit(id, ilen);
}

// Gutenberg Alice: .chapter { text-align: left; font-size: 150% } — not centered.
bool classIsChapterLeftTitle(const Tag& tag) {
  size_t clen = 0;
  const char* c = attrValue(tag, "class", &clen);
  if (!c || clen == 0) return false;
  // Exact-ish token "chapter" (class="chapter" or "... chapter ..."), not chapter-title.
  if (containsI(c, clen, "chapter-title") || containsI(c, clen, "chaptitle")) return false;
  // class equals "chapter" or contains it as a word-ish token
  if (ieq(c, clen, "chapter")) return true;
  // "chapter" surrounded by spaces / start / end
  for (size_t i = 0; i + 7 <= clen; ++i) {
    if ((i == 0 || c[i - 1] == ' ') && (i + 7 == clen || c[i + 7] == ' ' || c[i + 7] == '\t')) {
      if (ieq(c + i, 7, "chapter")) return true;
    }
  }
  return false;
}

// Gutenberg/Alice: float on wrapper (.figleft/.figright), not on <img>.
// Illuminae: .figure_float_right_briefing / .figure_float_left_email (underscore).
bool classSaysFloatLeft(const Tag& tag) {
  return attrHasClass(tag, "figleft") || attrHasClass(tag, "floatleft") || attrHasClass(tag, "float-left") ||
         attrHasClass(tag, "float_left") || attrHasClass(tag, "alignleft") || attrHasClass(tag, "align-left");
}
bool classSaysFloatRight(const Tag& tag) {
  return attrHasClass(tag, "figright") || attrHasClass(tag, "floatright") || attrHasClass(tag, "float-right") ||
         attrHasClass(tag, "float_right") || attrHasClass(tag, "alignright") || attrHasClass(tag, "align-right");
}

// Kindle dual-format pair: EPUB keeps .squeeze-epub; .squeeze-amzn* is display:none in CSS
// (and data-AmznRemoved-M8 marks the KF8-only twin). Showing both duplicates briefings (Illuminae).
bool isSuppressedKindleTwinImg(const Tag& tag) {
  size_t vlen = 0;
  const char* m8 = attrValue(tag, "data-AmznRemoved-M8", &vlen);
  if (m8 && vlen > 0) {
    // Any non-empty value (usually "true") → suppress this twin.
    return true;
  }
  // Class tokens: squeeze-amzn, squeeze-amzn1, … (display:none in publisher CSS).
  if (attrHasClass(tag, "squeeze-amzn")) return true;
  return false;
}

// Illuminae ships document UI as JPEGs with rich accessibility alts (not OCR).
// Classic path used alt as readable text; painting plates is dark/hard on e-ink.
// Patterns: Briefing note, AFTER ACTION headers, intercept captions, CLASSIFIED stamps.
bool looksLikeDocumentAlt(const char* alt, size_t altLen) {
  if (!alt || altLen < 12) return false;
  // Short stamps (CLASSIFIED GIFs) — still useful as text labels.
  if (altLen < 80 && containsI(alt, altLen, "CLASSIFIED")) return true;
  if (altLen < 48) return false;
  return containsI(alt, altLen, "Briefing note") || containsI(alt, altLen, "briefing note") ||
         containsI(alt, altLen, "document title") || containsI(alt, altLen, "AFTER ACTION") ||
         containsI(alt, altLen, "Intercepted Personal Message") || containsI(alt, altLen, "Surveillance footage") ||
         (containsI(alt, altLen, "Briefing") && containsI(alt, altLen, "paperclip")) ||
         (containsI(alt, altLen, "background insignia") && altLen >= 80) ||
         (containsI(alt, altLen, "stamped text") && altLen >= 40) ||
         (containsI(alt, altLen, "paperclip icon") && altLen >= 60);
}

// Collapse whitespace; prefer "Briefing note:" / document-title body when present.
// minOut: short stamps need a low floor; long notes use 24+.
bool buildReadableAltText(const char* alt, size_t altLen, std::string& out, size_t minOut = 24) {
  out.clear();
  if (!alt || altLen == 0) return false;
  size_t start = 0;
  // Prefer text from "Briefing note:" if present.
  for (size_t i = 0; i + 13 < altLen; ++i) {
    if (startsWithI(alt + i, alt + altLen, "Briefing note")) {
      start = i;
      break;
    }
  }
  // Else prefer "(document title) TITLE" → TITLE after the close paren.
  if (start == 0) {
    for (size_t i = 0; i + 15 < altLen; ++i) {
      if (startsWithI(alt + i, alt + altLen, "(document title)")) {
        size_t j = i + 15;
        while (j < altLen && (alt[j] == ' ' || alt[j] == '\t')) ++j;
        start = j;
        break;
      }
    }
  }
  if (!safeReserve(out, altLen + 8)) return false;
  bool space = false;
  bool any = false;
  for (size_t i = start; i < altLen; ++i) {
    const unsigned char c = static_cast<unsigned char>(alt[i]);
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      space = any;
      continue;
    }
    // Drop decorative "(background...)" / "(text inset...)" / "(stamped text)" wrappers
    // when we are still before the main body (start==0 path).
    if (c == '(' && start == 0) {
      size_t j = i + 1;
      while (j < altLen && alt[j] != ')') ++j;
      if (j < altLen) {
        // Keep short stamp content: "(stamped text) CLASSIFIED" → take after ')'.
        const size_t openLen = j - (i + 1);
        if (openLen >= 6 && openLen <= 40 && containsI(alt + i + 1, openLen, "stamped")) {
          i = j;
          space = any;
          continue;
        }
        if (openLen >= 6 && containsI(alt + i + 1, openLen, "background")) {
          i = j;
          space = any;
          continue;
        }
        if (openLen >= 6 && containsI(alt + i + 1, openLen, "text inset")) {
          i = j;
          space = any;
          continue;
        }
        if (openLen >= 6 && containsI(alt + i + 1, openLen, "document title")) {
          i = j;
          space = any;
          continue;
        }
      }
    }
    if (space) {
      if (!safePushChar(out, ' ')) return false;
      space = false;
    }
    if (!safePushChar(out, static_cast<char>(c))) return false;
    any = true;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  // Drop leading punctuation leftovers from stripped parens.
  while (!out.empty() && (out.front() == ',' || out.front() == ';' || out.front() == ' ')) {
    out.erase(out.begin());
  }
  return out.size() >= minOut;
}

// Parse CSS width: NNpx from style= (Alice figleft style="width: 80px").
int parseStyleWidthPx(const Tag& tag) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "style", &vlen);
  if (!v || vlen < 8) return 0;
  // Find "width:" then integer px.
  for (size_t i = 0; i + 6 < vlen; ++i) {
    if ((v[i] == 'w' || v[i] == 'W') && i + 5 < vlen) {
      // crude case-insensitive "width"
      const char* w = "width";
      bool ok = true;
      for (int k = 0; k < 5; ++k) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(v[i + k])));
        if (ca != w[k]) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      size_t j = i + 5;
      while (j < vlen && (v[j] == ' ' || v[j] == ':')) ++j;
      int px = 0;
      size_t digits = 0;
      while (j < vlen && v[j] >= '0' && v[j] <= '9' && digits < 4) {
        px = px * 10 + (v[j] - '0');
        ++j;
        ++digits;
      }
      if (px > 0 && px < 2000) return px;
    }
  }
  return 0;
}

// Parse inline style= for weight / style / size / align (thin CSS).
void applyInlineStyle(const Tag& tag, RunStyle& styleInOut, SizeStep& sizeInOut, Align* alignOut = nullptr) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "style", &vlen);
  if (!v || vlen == 0) return;
  if (containsI(v, vlen, "font-weight:bold") || containsI(v, vlen, "font-weight: bold") ||
      containsI(v, vlen, "font-weight:700") || containsI(v, vlen, "font-weight: 700") ||
      containsI(v, vlen, "font-weight:600") || containsI(v, vlen, "font-weight:800")) {
    // Bit-set, not value-replace: RunStyle is a bitmask, so the old value tests
    // dropped any decoration already carried on the run.
    styleInOut |= RunStyle::Bold;
  }
  if (containsI(v, vlen, "font-style:italic") || containsI(v, vlen, "font-style: italic") ||
      containsI(v, vlen, "font-style:oblique")) {
    styleInOut |= RunStyle::Italic;
  }
  // DCC / Butcher ordinals:
  //   6<span style="font-size:0.75em; margin-left:0.05em; vertical-align:super">th</span>
  // Paint already scales SUP/SUB to ~50% and raises the baseline; sizeStep on
  // the same span would shrink twice. Classic ChapterHtmlSlimParser glues these
  // the same way.
  const bool isSuper = containsI(v, vlen, "vertical-align:super") || containsI(v, vlen, "vertical-align: super");
  const bool isSub = containsI(v, vlen, "vertical-align:sub") || containsI(v, vlen, "vertical-align: sub");
  if (isSuper) styleInOut |= RunStyle::Superscript;
  if (isSub) styleInOut |= RunStyle::Subscript;
  // Size bumps never shrink an already-larger step (h1 Plus2 must stick).
  if (!isSuper && !isSub) {
    if (containsI(v, vlen, "font-size:2em") || containsI(v, vlen, "font-size: 2em") || containsI(v, vlen, "xx-large") ||
        containsI(v, vlen, "2.0em") || containsI(v, vlen, "font-size:1.6") || containsI(v, vlen, "font-size:1.5")) {
      if (sizeInOut < SizeStep::Plus2) sizeInOut = SizeStep::Plus2;
    } else if (containsI(v, vlen, "font-size:1.4") || containsI(v, vlen, "font-size:1.3") ||
               containsI(v, vlen, "font-size:1.2") || containsI(v, vlen, "x-large") ||
               containsI(v, vlen, "font-size:large")) {
      if (sizeInOut < SizeStep::Plus1) sizeInOut = SizeStep::Plus1;
    }
    if (containsI(v, vlen, "font-size:small") || containsI(v, vlen, "font-size:0.8") ||
        containsI(v, vlen, "font-size:0.9") || containsI(v, vlen, "font-size:0.85")) {
      sizeInOut = SizeStep::Minus1;
    }
  }
  if (alignOut) {
    if (containsI(v, vlen, "text-align:center") || containsI(v, vlen, "text-align: center")) {
      *alignOut = Align::Center;
    } else if (containsI(v, vlen, "text-align:right") || containsI(v, vlen, "text-align: right")) {
      *alignOut = Align::Right;
    } else if (containsI(v, vlen, "text-align:left") || containsI(v, vlen, "text-align: left")) {
      *alignOut = Align::Left;
    }
  }
}

// class tokens that imply emphasis
void applyClassEmphasis(const Tag& tag, RunStyle& styleInOut, SizeStep& sizeInOut) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "class", &vlen);
  if (!v || vlen == 0) return;
  if (containsI(v, vlen, "bold") || containsI(v, vlen, "strong") || containsI(v, vlen, "bolder")) {
    styleInOut |= RunStyle::Bold;
  }
  if (containsI(v, vlen, "italic") || containsI(v, vlen, "oblique") || containsI(v, vlen, "emphasis") ||
      containsI(v, vlen, "emph") || containsI(v, vlen, "cite")) {
    styleInOut |= RunStyle::Italic;
  }
  // Alice .chapter is larger regular, not bold — skip bold promotion.
  if (looksLikeTitleHost(tag) && !classIsChapterLeftTitle(tag)) {
    if (sizeInOut < SizeStep::Plus1) sizeInOut = SizeStep::Plus1;
    styleInOut |= RunStyle::Bold;
  } else if (classIsChapterLeftTitle(tag)) {
    if (sizeInOut < SizeStep::Plus1) sizeInOut = SizeStep::Plus1;
  }
}

// Peek the inner text of an open inline tag. First-letter polyfills wrap only
// "[" of DCC "Chapter [1]" / "[ 1 ]"; applying bold/size there leaves one
// heavy bracket. Abort quickly on a letter/digit so this is cheap per span.
bool innerIsOnlyOpeningPunct(const char* p, const char* end, const char* closeName, const size_t closeNameLen) {
  if (!p || !closeName || closeNameLen == 0 || closeNameLen >= 8) return false;
  char close[8];
  std::memcpy(close, closeName, closeNameLen);
  close[closeNameLen] = '\0';
  int depth = 1;
  bool sawPunct = false;
  int walked = 0;
  constexpr int kPeekLimit = 64;
  while (p < end && depth > 0 && walked < kPeekLimit) {
    if (*p == '<') {
      Tag tag;
      const size_t used = parseTag(p, end, tag);
      if (used == 0) return false;
      if (tag.closing) {
        // The matching close must be this tag's name; a stray </p> is not the end.
        if (depth == 1 && !ieq(tag.name, tag.nameLen, close)) return false;
        --depth;
      } else if (!tag.selfClose) {
        ++depth;
      }
      p += used;
      walked += static_cast<int>(used);
      continue;
    }
    if (*p == '&') {
      std::string tmp;
      tmp.reserve(8);
      bool oom = false;
      const size_t n = decodeEntity(p, end, tmp, &oom);
      if (!n || oom || tmp.empty()) return false;
      if (!punctemph::textIsOnlyOpeningPunctuation(tmp.data(), tmp.size())) return false;
      sawPunct = true;
      p += n;
      walked += static_cast<int>(n);
      continue;
    }
    if (punctemph::isAsciiWs(*p)) {
      ++p;
      ++walked;
      continue;
    }
    const unsigned char* up = reinterpret_cast<const unsigned char*>(p);
    const uint32_t cp = utf8NextCodepoint(&up);
    if (cp == 0 || reinterpret_cast<const char*>(up) <= p) return false;
    if (!punctemph::isOpeningPunctuation(cp)) return false;
    sawPunct = true;
    walked += static_cast<int>(reinterpret_cast<const char*>(up) - p);
    p = reinterpret_cast<const char*>(up);
  }
  return sawPunct && depth == 0;
}

SizeStep sizeForHeading(const int level) {
  // Match classic StyleResolve::sizeStepForHeadingLevel (Literata ladder).
  // h1 → +2 (e.g. 12→16), h2 → +1 (12→14), h3+ → body.
  // Applied at IR build AND re-asserted in PageLayouter for heading blocks.
  if (level <= 1) return SizeStep::Plus2;
  if (level == 2) return SizeStep::Plus1;
  return SizeStep::Body;
}

bool styleSaysCenter(const Tag& tag) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "style", &vlen);
  if (v && vlen && (containsI(v, vlen, "text-align:center") || containsI(v, vlen, "text-align: center"))) {
    return true;
  }
  size_t clen = 0;
  const char* c = attrValue(tag, "class", &clen);
  if (!c || clen == 0) return false;
  // Eye of the Bedlam Bride: .alignment-block-content { text-align: center }
  // Avoid bare "title"/"chapter" — those match layout wrappers and over-center body.
  return containsI(c, clen, "center") || containsI(c, clen, "centred") || containsI(c, clen, "centered") ||
         containsI(c, clen, "alignment-block") || containsI(c, clen, "aligncenter") ||
         containsI(c, clen, "text-center") || containsI(c, clen, "text_center") || containsI(c, clen, "toc");
}

// Legacy HTML align="center|left|right|justify" on p/h1/div/td/img.
// Classic ChapterHtmlSlimParser always honors this (even with stylesheets off) —
// many EPUBs center chapter titles that way without a class or inline CSS.
bool htmlAlignAttr(const Tag& tag, Align& out) {
  size_t vlen = 0;
  const char* v = attrValue(tag, "align", &vlen);
  if (!v || vlen == 0) return false;
  if (ieq(v, vlen, "center") || ieq(v, vlen, "middle")) {
    out = Align::Center;
    return true;
  }
  if (ieq(v, vlen, "right")) {
    out = Align::Right;
    return true;
  }
  if (ieq(v, vlen, "left")) {
    out = Align::Left;
    return true;
  }
  if (ieq(v, vlen, "justify")) {
    out = Align::Justify;
    return true;
  }
  return false;
}

bool classSaysNoIndent(const Tag& tag) {
  // Illuminae memos/emails: .nonindent / .nonindent-em / .list_ul (bullet lines).
  return attrHasClass(tag, "unindent") || attrHasClass(tag, "noindent") || attrHasClass(tag, "no-indent") ||
         attrHasClass(tag, "nonindent") || attrHasClass(tag, "nonindent-em") || attrHasClass(tag, "list_ul") ||
         attrHasClass(tag, "first2") || attrHasClass(tag, "first3") || attrHasClass(tag, "first4") ||
         attrHasClass(tag, "first5") ||  // Bedlam: first para after title, indent 0
         attrHasClass(tag, "alignment-block-content");
}

// Book-style vertical rhythm: CSS em × this scale → IR Q4.
// Full 1:1 CSS em is too airy on a ~5" e-ink page (1.4em gaps stacked to ¾").
// ~0.4 keeps relative hierarchy (big break > small break) while densifying.
// Adjacent spacers also margin-collapse in PageLayouter (CSS adjacent-margin rule).
constexpr int kBookStyleVSpaceScaleQ8 = 102;  // 102/256 ≈ 0.40

// Scale a CSS em (in 1/16 em units) for book-style spacers.
int scaleBookVSpaceQ4(const int cssEmQ4) {
  if (cssEmQ4 <= 0) return 0;
  const int scaled = (cssEmQ4 * kBookStyleVSpaceScaleQ8 + 128) / 256;
  return std::max(2, std::min(127, scaled));  // floor ~0.125em so gaps stay visible
}

// Eye of the Bedlam Bride (and similar Calibre): empty vertical rhythm blocks.
// CSS: .implicit-break { margin-top: 1.4em; height: 0 }
//      .implicit-break1 { height: 1.4em; margin-bottom: 1.4em }
// Values stored pre-scale; scaleBookVSpaceQ4 applied at emit.
bool classIsImplicitBreak(const Tag& tag, int& outTopEmQ4, int& outBottomEmQ4) {
  outTopEmQ4 = 0;
  outBottomEmQ4 = 0;
  if (attrHasClass(tag, "implicit-break1")) {
    // CSS height 1.4em + margin-bottom 1.4em → 2.8em raw, then book scale.
    outTopEmQ4 = 0;
    outBottomEmQ4 = scaleBookVSpaceQ4(45);  // 2.8em × ~0.4
    return true;
  }
  if (attrHasClass(tag, "implicit-break")) {
    outTopEmQ4 = scaleBookVSpaceQ4(22);  // 1.4em × ~0.4
    outBottomEmQ4 = 0;
    return true;
  }
  return false;
}

bool classIsAlignmentBlockContent(const Tag& tag) { return attrHasClass(tag, "alignment-block-content"); }

bool classIsBodyFirst(const Tag& tag) {
  // first2/first3: no indent, body size, margin 0 (Bedlam system + narrative open)
  return attrHasClass(tag, "first2") || attrHasClass(tag, "first3") || attrHasClass(tag, "first4") ||
         attrHasClass(tag, "first5");
}

bool classIsSubsequent(const Tag& tag) {
  // subsq: 1.5em indent, body size, margin 0
  return attrHasClass(tag, "subsq") || attrHasClass(tag, "subsq1") || attrHasClass(tag, "subsq2");
}

struct StyleFrame {
  RunStyle style = RunStyle::Regular;
  SizeStep size = SizeStep::Body;
};

}  // namespace

bool HtmlToIr::convert(const char* html, const size_t len, ChapterIr& out, const bool armDropCapOnFirstParagraph,
                       const uint8_t imageRendering) {
  out.clear();
  if (!html || len == 0) return false;
  // Caller still holds HTML. free/maxA are net of that buffer. Only refuse when
  // contiguous heap is tiny (crash was abort at maxA≈15KB with unchecked strings;
  // growth is now heap-checked — soft fail, not device abort).
  if (ESP.getMaxAllocHeap() < 10 * 1024 || ESP.getFreeHeap() < 12 * 1024) {
    LOG_ERR("RVIR", "convert refuse: free=%u maxA=%u html=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(len));
    return false;
  }
  // Pre-size text/runs/blocks once (malloc/realloc, heap-checked) so convert does
  // not fragment mid-chapter — that produced partial IR and a false "last page".
  out.reserveForConvert(len);
  const char* p = html;
  const char* end = html + len;

  for (const char* s = p; s + 6 < end; ++s) {
    if (startsWithI(s, end, "<body")) {
      while (s < end && *s != '>') ++s;
      if (s < end) ++s;
      p = s;
      break;
    }
  }

  bool dropCapHost = false;
  bool dropCapArmed = armDropCapOnFirstParagraph;
  bool inSkip = false;  // script/style/svg
  int skipDepth = 0;
  bool inHidden = false;  // oculto / sr-only / etc.
  int hiddenDepth = 0;
  int titleDivDepth = 0;
  // Alice/Gutenberg: float side + optional CSS width inherit from .figleft wrapper.
  int floatInherit = 0;  // 0 none, 1 left, 2 right
  int floatWidthPx = 0;
  int floatDivDepth = 0;
  bool unindentInherit = false;
  int unindentDivDepth = 0;
  // Bedlam .alignment-block vertical margins (class often missing on </div>).
  int alignmentBlockDepth = 0;
  // Open heading level 1–6 (0 = none). Soft <br> keeps the same heading block style.
  int headingLevelOpen = 0;
  Align headingAlignOpen = Align::Center;
  // Fourth Wing: .orn span/div wraps a small chapter ornament image (~12% width).
  int ornamentDepth = 0;
  // Epigraph / citaini blockquote — center + slightly smaller body.
  int epigraphDepth = 0;
  // <hgroup> depth (Standard Ebooks chapter titles).
  int hgroupDepth = 0;
  // Book Style CSS approximation: h2+p, hgroup+p, hr+p, p:first-child → text-indent: 0.
  // Armed when a heading/hgroup/hr ends; consumed by the next body <p>.
  bool noIndentNextParagraph = false;
  // >0 inside a <table>; >1 means a nested table whose content is discarded.
  int tableDepth = 0;

  // Style stack is block-scoped. Unclosed <span>/<b>/<i> used to leak frames;
  // after silent push failures the stuck face (often Bold from an <h1>) painted
  // the rest of the chapter bold — classic "OH, MAN," normal then everything bold.
  // 32 frames is cheap (2 bytes each) and covers deeply nested EPUB emphasis
  // without dropping styles. On overflow we OR the new face onto the top rather
  // than replacing it, so bold/italic cannot silently vanish mid-chapter.
  StyleFrame stack[32];
  int stackTop = 0;
  int styleFloor = 0;  // never pop below this (current block's base face)
  bool styleOverflowLogged = false;
  stack[0] = {};

  auto curStyle = [&]() -> StyleFrame& { return stack[stackTop]; };

  auto pushStyle = [&](const RunStyle st, const SizeStep sz) {
    if (stackTop + 1 < 32) {
      ++stackTop;
      stack[stackTop].style = st;
      stack[stackTop].size = sz;
      return;
    }
    // Stack full: merge onto the top. Replacing used to drop the accumulated
    // face when a deep nest pushed Bold over Italic (or vice versa) and later
    // pops could not unwind — styles looked like they "stopped working".
    stack[stackTop].style = stack[stackTop].style | st;
    if (static_cast<int>(sz) > static_cast<int>(stack[stackTop].size)) {
      stack[stackTop].size = sz;
    }
    if (!styleOverflowLogged) {
      styleOverflowLogged = true;
      LOG_DBG("RVIR", "style stack full — merging faces (chapter has deep nesting)");
    }
  };
  auto popStyle = [&]() {
    // Never pop through the current block's base face (orphan </span> after
    // self-closing page anchors used to peel the paragraph Regular off).
    if (stackTop > styleFloor) --stackTop;
  };
  // Start a block with a known face — drops any leaked inline frames.
  auto beginBlockStyle = [&](const RunStyle st, const SizeStep sz) {
    stackTop = 0;
    stack[0] = {RunStyle::Regular, SizeStep::Body};
    styleFloor = 0;
    pushStyle(st, sz);
    styleFloor = stackTop;
  };
  auto endBlockStyle = [&]() {
    stackTop = 0;
    styleFloor = 0;
    stack[0] = {RunStyle::Regular, SizeStep::Body};
  };

  bool inBlock = false;
  std::string textAcc;
  // Cap accumulator; flush often so we never need a multi-KB unchecked grow.
  if (!safeReserve(textAcc, 512)) {
    LOG_ERR("RVIR", "textAcc reserve fail free=%u maxA=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }

  auto flushText = [&]() {
    if (out.failed() || textAcc.empty() || !inBlock) {
      textAcc.clear();
      return;
    }
    // Only strip leading spaces on the first run of a block. After a style change
    // (</i> skill, </em> next) the inter-word space is often the first char of the
    // next run — stripping it glued "Vampire"+"skill" / italic+roman words.
    const bool firstRun = out.blocks().empty() || out.blocks().back().runCount == 0;
    size_t i = 0;
    if (firstRun) {
      while (i < textAcc.size() && textAcc[i] == ' ') ++i;
    } else if (textAcc[0] == ' ') {
      // Collapse "   word" → " word" but keep one leading space.
      while (i + 1 < textAcc.size() && textAcc[i + 1] == ' ') ++i;
    }
    if (i >= textAcc.size()) {
      textAcc.clear();
      return;
    }
    if (!out.appendRun(curStyle().style, curStyle().size, textAcc.data() + i, textAcc.size() - i)) {
      textAcc.clear();
      return;
    }
    textAcc.clear();
  };

  auto openBlock = [&](const BlockKind kind, const Align align, const uint16_t flags) {
    if (out.failed()) return;
    flushText();
    if (inBlock) out.endBlock();
    const size_t before = out.blockCount();
    out.beginBlock(kind, align, flags);
    inBlock = !out.failed() && out.blockCount() > before;
  };

  auto closeBlock = [&]() {
    flushText();
    if (inBlock) {
      out.endBlock();
      inBlock = false;
    }
  };

  // Bit operations, not value comparisons: RunStyle is a bitmask now, so the old
  // `base == RunStyle::Italic` style tests would have silently dropped any
  // decoration already on the frame (bold inside underline lost the underline).
  auto mergeBold = [&](RunStyle base) { return base | RunStyle::Bold; };
  auto mergeItalic = [&](RunStyle base) { return base | RunStyle::Italic; };
  auto mergeDecoration = [&](RunStyle base, RunStyle bit) { return base | bit; };

  while (p < end && !out.failed()) {
    if (*p == '<') {
      Tag tag;
      const size_t used = parseTag(p, end, tag);
      if (used == 0) {
        ++p;
        continue;
      }
      p += used;
      if (tag.nameLen == 0) continue;

      if (ieq(tag.name, tag.nameLen, "script") || ieq(tag.name, tag.nameLen, "style") ||
          ieq(tag.name, tag.nameLen, "svg")) {
        if (!tag.closing && !tag.selfClose) {
          inSkip = true;
          skipDepth = 1;
        } else if (tag.closing) {
          inSkip = false;
          skipDepth = 0;
        }
        continue;
      }
      if (inSkip) {
        if (!tag.closing && !tag.selfClose)
          ++skipDepth;
        else if (tag.closing && --skipDepth <= 0) {
          inSkip = false;
          skipDepth = 0;
        }
        continue;
      }

      // Skip visually-hidden hosts entirely (e.g. <h1 class="oculto">Chapter 1</h1>).
      // Also HTML hidden= on <nav> (landmarks/page-list) and style=display:none spans.
      if (!tag.closing && !tag.selfClose && isHiddenHost(tag)) {
        inHidden = true;
        hiddenDepth = 1;
        continue;
      }
      if (inHidden) {
        if (!tag.closing && !tag.selfClose)
          ++hiddenDepth;
        else if (tag.closing && --hiddenDepth <= 0) {
          inHidden = false;
          hiddenDepth = 0;
        }
        continue;
      }

      // EPUB nav / HTML lists: each <li> is its own line (Isako Contents page uses
      // <nav><ol><li>…</li></ol></nav>). Without this, all chapter titles run into
      // one paragraph because we only broke on <p>/<h*>.
      if (!tag.closing && !tag.selfClose &&
          (ieq(tag.name, tag.nameLen, "nav") || ieq(tag.name, tag.nameLen, "ol") || ieq(tag.name, tag.nameLen, "ul"))) {
        closeBlock();
        continue;
      }
      if (tag.closing &&
          (ieq(tag.name, tag.nameLen, "nav") || ieq(tag.name, tag.nameLen, "ol") || ieq(tag.name, tag.nameLen, "ul"))) {
        closeBlock();
        continue;
      }
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "li")) {
        closeBlock();
        // TOC / list lines: no first-line indent, tight stack.
        openBlock(BlockKind::Paragraph, Align::Left, kBlockNoIndent);
        out.setCurrentMarginsEmQ4(0, 2);
        continue;
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "li")) {
        closeBlock();
        continue;
      }

      // Tables. Rivulet has no table block kind and building real column layout
      // on a 480px panel is not worth it, so mirror exactly what the classic
      // engine did: stream each cell as its own block, no "Row N Cell M" chrome,
      // and discard nested tables. Without this, table/tr/td/th were simply
      // unknown tags and every cell ran together into one unreadable paragraph -
      // many EPUBs use tables purely as layout vehicles for caption/image pairs.
      if (ieq(tag.name, tag.nameLen, "table")) {
        if (!tag.closing && !tag.selfClose) {
          closeBlock();
          ++tableDepth;
        } else if (tag.closing && tableDepth > 0) {
          closeBlock();
          --tableDepth;
        }
        continue;
      }
      if (tableDepth > 1) {
        // Inside a nested table: drop its markup and text entirely (classic parity).
        continue;
      }
      if (tableDepth == 1 &&
          (ieq(tag.name, tag.nameLen, "td") || ieq(tag.name, tag.nameLen, "th") || ieq(tag.name, tag.nameLen, "tr"))) {
        closeBlock();
        if (!tag.closing && !tag.selfClose && (ieq(tag.name, tag.nameLen, "td") || ieq(tag.name, tag.nameLen, "th"))) {
          // One cell = one block. Tight stack, no first-line indent, same as <li>.
          Align cellAlign = Align::Left;
          if (styleSaysCenter(tag)) cellAlign = Align::Center;
          Align htmlA = Align::Left;
          if (htmlAlignAttr(tag, htmlA)) cellAlign = htmlA;
          RunStyle dummySt = RunStyle::Regular;
          SizeStep dummySz = SizeStep::Body;
          applyInlineStyle(tag, dummySt, dummySz, &cellAlign);
          openBlock(BlockKind::Paragraph, cellAlign, kBlockNoIndent);
          out.setCurrentMarginsEmQ4(0, 2);
        }
        continue;
      }

      // Drop-cap host open (explicit only — not every blockquote/epigraph).
      if (!tag.closing && (ieq(tag.name, tag.nameLen, "blockquote") || ieq(tag.name, tag.nameLen, "div"))) {
        if (attrHasClass(tag, "ct1") || attrHasClass(tag, "dropcap") || attrHasClass(tag, "drop-cap") ||
            attrHasClass(tag, "firstletter") || attrHasClass(tag, "first-letter")) {
          dropCapHost = true;
        }
      }
      if (tag.closing && (ieq(tag.name, tag.nameLen, "blockquote") || ieq(tag.name, tag.nameLen, "div"))) {
        if (dropCapHost) {
          dropCapArmed = true;
          dropCapHost = false;
        }
      }

      // Epigraph / citaini (Fourth Wing chapter open quotes).
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "blockquote")) {
        if (attrHasClass(tag, "citaini") || attrHasClass(tag, "epigraph") || attrHasClass(tag, "epigrafe") ||
            attrHasClass(tag, "quote") || attrHasClass(tag, "cita")) {
          epigraphDepth = 1;
        } else if (epigraphDepth > 0) {
          ++epigraphDepth;
        }
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "blockquote") && epigraphDepth > 0) {
        if (--epigraphDepth <= 0) epigraphDepth = 0;
      }

      // .orn wrapper (chapter ornament image, often inside h1).
      if (!tag.closing && !tag.selfClose && (ieq(tag.name, tag.nameLen, "span") || ieq(tag.name, tag.nameLen, "div"))) {
        if (attrHasClass(tag, "orn") || attrHasClass(tag, "ornament") || attrHasClass(tag, "chapter-orn")) {
          ornamentDepth = 1;
        } else if (ornamentDepth > 0) {
          ++ornamentDepth;
        }
      }
      if (tag.closing && (ieq(tag.name, tag.nameLen, "span") || ieq(tag.name, tag.nameLen, "div")) &&
          ornamentDepth > 0) {
        if (--ornamentDepth <= 0) ornamentDepth = 0;
      }

      // figleft / figright / unindent wrapper open/close (Alice letter glyphs).
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "div")) {
        if (classSaysFloatLeft(tag) || classSaysFloatRight(tag)) {
          floatInherit = classSaysFloatRight(tag) ? 2 : 1;
          floatWidthPx = parseStyleWidthPx(tag);
          floatDivDepth = 1;
        } else if (floatDivDepth > 0) {
          ++floatDivDepth;
        }
        if (attrHasClass(tag, "unindent") || attrHasClass(tag, "noindent") || attrHasClass(tag, "no-indent")) {
          unindentInherit = true;
          unindentDivDepth = 1;
        } else if (unindentDivDepth > 0) {
          ++unindentDivDepth;
        }
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "div")) {
        if (floatDivDepth > 0 && --floatDivDepth <= 0) {
          floatInherit = 0;
          floatWidthPx = 0;
          floatDivDepth = 0;
        }
        if (unindentDivDepth > 0 && --unindentDivDepth <= 0) {
          unindentInherit = false;
          unindentDivDepth = 0;
        }
      }

      // <hgroup> (Standard Ebooks): wraps h2 + title.
      // CSS: hgroup { margin: 3em 0 } but children { margin: 0 }; hgroup + p { text-indent: 0 }.
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "hgroup")) {
        closeBlock();
        ++hgroupDepth;
        continue;
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "hgroup")) {
        if (hgroupDepth > 0) --hgroupDepth;
        closeBlock();
        // One shot of air after the whole title block (not between "I" and subtitle).
        // CSS 3em bottom × book scale ≈ decent chapter-open gap before body.
        openBlock(BlockKind::Spacer, Align::Left, kBlockNoIndent);
        out.setCurrentMarginsEmQ4(0, static_cast<int8_t>(scaleBookVSpaceQ4(48)));  // 3em scaled
        closeBlock();
        // CSS: hgroup + p { text-indent: 0 }
        noIndentNextParagraph = true;
        continue;
      }

      // Headings h1–h6
      if (!tag.closing && tag.nameLen == 2 && (tag.name[0] == 'h' || tag.name[0] == 'H') && tag.name[1] >= '1' &&
          tag.name[1] <= '6') {
        const int level = tag.name[1] - '0';
        closeBlock();
        const BlockKind bk = static_cast<BlockKind>(static_cast<uint8_t>(BlockKind::Heading1) + (level - 1));
        Align headingAlign = Align::Center;
        Align htmlA = Align::Center;
        if (htmlAlignAttr(tag, htmlA)) headingAlign = htmlA;
        RunStyle st = RunStyle::Bold;
        SizeStep sz = sizeForHeading(level);
        applyInlineStyle(tag, st, sz, &headingAlign);
        openBlock(bk, headingAlign, kBlockNoIndent);
        if (hgroupDepth > 0) {
          // SE: hgroup > * { margin: 0 } — air comes from hgroup close spacer only.
          out.setCurrentMarginsEmQ4(0, 0);
        } else {
          // Standalone heading: modest top, real bottom gap before body (scaled ~1.5–2em).
          out.setCurrentMarginsEmQ4(level <= 1 ? 6 : 4, static_cast<int8_t>(scaleBookVSpaceQ4(32)));
        }
        // element-title 1.29em etc. may bump size; keep within ladder (±2).
        if (sz > SizeStep::Plus2) sz = SizeStep::Plus2;
        beginBlockStyle(st, sz);
        headingLevelOpen = level;
        headingAlignOpen = headingAlign;
        continue;
      }
      if (tag.closing && tag.nameLen == 2 && (tag.name[0] == 'h' || tag.name[0] == 'H') && tag.name[1] >= '1' &&
          tag.name[1] <= '6') {
        // Flush while the heading face is still on the stack. endBlockStyle first
        // used to dump the trailing run as Regular, so a first-letter span around
        // "[" of "[ 1 ]" stayed Bold and "1]" went Regular.
        closeBlock();
        endBlockStyle();
        headingLevelOpen = 0;
        // CSS: h2 + p, h3 + p, … { text-indent: 0 } — only when not inside hgroup
        // (hgroup's own close arms the flush for the first body para after the group).
        if (hgroupDepth == 0) noIndentNextParagraph = true;
        continue;
      }

      // Re-open the current heading after an ornament image or soft <br>.
      // Style frame stays on the stack from the original <hN> open (no extra push).
      auto reopenHeadingIfNeeded = [&]() {
        if (headingLevelOpen < 1 || headingLevelOpen > 6) return;
        const BlockKind bk = static_cast<BlockKind>(static_cast<uint8_t>(BlockKind::Heading1) + (headingLevelOpen - 1));
        openBlock(bk, headingAlignOpen, kBlockNoIndent);
        out.setCurrentMarginsEmQ4(2, 12);  // tight top after ornament / soft break
      };

      // Paragraph — title heuristics on class/id
      if (!tag.closing && ieq(tag.name, tag.nameLen, "p")) {
        closeBlock();
        // Empty vertical rhythm (Bedlam .implicit-break / .implicit-break1).
        int spTop = 0, spBot = 0;
        if (classIsImplicitBreak(tag, spTop, spBot)) {
          openBlock(BlockKind::Spacer, Align::Left, kBlockNoIndent);
          out.setCurrentMarginsEmQ4(static_cast<int8_t>(std::min(127, spTop)),
                                    static_cast<int8_t>(std::min(127, spBot)));
          closeBlock();
          continue;
        }
        uint16_t flags = 0;
        // Default justify — matches classic "book" feel; user force-align overrides in layouter.
        Align align = Align::Justify;
        BlockKind kind = BlockKind::Paragraph;
        // Body size for system/body paras — do not inherit a leftover heading size step.
        RunStyle st = RunStyle::Regular;
        SizeStep sz = SizeStep::Body;
        // Epigraph / citaini (Fourth Wing): center, slightly smaller, no indent.
        if (epigraphDepth > 0 || attrHasClass(tag, "firma") || attrHasClass(tag, "citaini")) {
          align = Align::Center;
          flags |= kBlockNoIndent;
          sz = SizeStep::Minus1;  // CSS ~0.825em
          if (attrHasClass(tag, "firma")) st = RunStyle::Bold;
        } else if (styleSaysCenter(tag) || classIsAlignmentBlockContent(tag)) {
          align = Align::Center;
          flags |= kBlockNoIndent;
          // CSS .alignment-block-content { margin: 0; font-size: 1em } — same size as body.
          sz = SizeStep::Body;
        } else if (classIsChapterLeftTitle(tag)) {
          align = Align::Left;
          flags |= kBlockNoIndent;
          if (sz < SizeStep::Plus1) sz = SizeStep::Plus1;  // ~150%
        } else if (looksLikeTitleHost(tag)) {
          // Epigraphs / captions: often centered in tradepub CSS we don't parse fully.
          align = Align::Center;
          flags |= kBlockNoIndent;
          if (sz < SizeStep::Plus1) sz = SizeStep::Plus1;
        }
        if (classSaysNoIndent(tag) || unindentInherit || classIsBodyFirst(tag)) {
          flags |= kBlockNoIndent;
        }
        // Standard Ebooks / tradepub adjacency (core.css):
        //   h2+p, hgroup+p, hr+p, p.first-child, p.continued { text-indent: 0 }
        // Without full CSS, approximate with "next para after heading/hgroup/hr".
        if (noIndentNextParagraph) {
          flags |= kBlockNoIndent;
          noIndentNextParagraph = false;
        }
        // Paras inside <hgroup> are titles (SE: hgroup > p { text-indent: 0 }).
        if (hgroupDepth > 0) {
          flags |= kBlockNoIndent;
          if (align == Align::Justify) align = Align::Center;
        }
        // Explicit SE/calibre flush classes.
        if (attrHasClass(tag, "continued") || attrHasClass(tag, "first-child") || attrHasClass(tag, "firstchild") ||
            attrHasClass(tag, "noindent") || attrHasClass(tag, "no-indent")) {
          flags |= kBlockNoIndent;
        }
        if (dropCapArmed && kind == BlockKind::Paragraph) {
          flags |= kBlockDropCap | kBlockNoIndent;
          dropCapArmed = false;
        }
        Align htmlA = Align::Left;
        if (htmlAlignAttr(tag, htmlA)) {
          align = htmlA;
          if (align == Align::Center || align == Align::Right) flags |= kBlockNoIndent;
        }
        applyInlineStyle(tag, st, sz, &align);
        applyClassEmphasis(tag, st, sz);
        // System lines stay body even if a class looked "title-ish".
        if (classIsAlignmentBlockContent(tag) || classIsBodyFirst(tag) || classIsSubsequent(tag)) {
          sz = SizeStep::Body;
        }
        openBlock(kind, align, flags);
        // Book-style margins: SE/core.css p { margin: 0 } — vertical rhythm is line-height
        // only between body lines. Space after chapter titles comes from hgroup/heading, not p.
        if (hgroupDepth > 0) {
          // SE: hgroup > * { margin: 0 }; air after whole group is the close spacer.
          out.setCurrentMarginsEmQ4(0, 0);
          out.setCurrentIndentEmQ4(0);
        } else if (epigraphDepth > 0 || attrHasClass(tag, "firma") || attrHasClass(tag, "citaini")) {
          // .citaini { margin: 2rem 1.5em 0 }; .firma { margin: 0.5rem 0 1rem }
          if (attrHasClass(tag, "firma")) {
            out.setCurrentMarginsEmQ4(static_cast<int8_t>(scaleBookVSpaceQ4(8)),
                                      static_cast<int8_t>(scaleBookVSpaceQ4(16)));
          } else {
            out.setCurrentMarginsEmQ4(static_cast<int8_t>(scaleBookVSpaceQ4(16)),
                                      static_cast<int8_t>(scaleBookVSpaceQ4(4)));
          }
          out.setCurrentIndentEmQ4(0);
        } else if (classIsAlignmentBlockContent(tag)) {
          out.setCurrentMarginsEmQ4(0, 0);
          out.setCurrentIndentEmQ4(0);
        } else if (classIsBodyFirst(tag) || classIsSubsequent(tag) || (flags & kBlockNoIndent) != 0 ||
                   align == Align::Center) {
          // Body / flush-first / center: no extra block gap (indent alone distinguishes).
          out.setCurrentMarginsEmQ4(0, 0);
          out.setCurrentIndentEmQ4((flags & kBlockNoIndent) != 0 || align == Align::Center ? 0
                                                                                           : static_cast<uint8_t>(16));
        } else {
          out.setCurrentMarginsEmQ4(0, 0);
          out.setCurrentIndentEmQ4(16);  // p { text-indent: 1em }
        }
        if (flags & kBlockDropCap) out.markDropCapOnCurrent();
        // Always reset stack for this paragraph — never inherit a leaked heading Bold.
        beginBlockStyle(st, sz);
        continue;
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "p")) {
        closeBlock();
        endBlockStyle();
        continue;
      }

      // .alignment-block { margin-top/bottom: 1.4em } — group air around Views/Bounty.
      // Scaled like other book-style spacers; collapses with neighboring breaks in layouter.
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "div") && attrHasClass(tag, "alignment-block")) {
        closeBlock();
        openBlock(BlockKind::Spacer, Align::Left, kBlockNoIndent);
        out.setCurrentMarginsEmQ4(static_cast<int8_t>(scaleBookVSpaceQ4(22)), 0);
        closeBlock();
        alignmentBlockDepth = 1;
        continue;
      }
      if (!tag.closing && !tag.selfClose && ieq(tag.name, tag.nameLen, "div") && alignmentBlockDepth > 0) {
        ++alignmentBlockDepth;
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "div") && alignmentBlockDepth > 0) {
        if (--alignmentBlockDepth <= 0) {
          alignmentBlockDepth = 0;
          closeBlock();
          openBlock(BlockKind::Spacer, Align::Left, kBlockNoIndent);
          out.setCurrentMarginsEmQ4(0, static_cast<int8_t>(scaleBookVSpaceQ4(22)));
          closeBlock();
        }
        // Fall through — other div handlers may also apply.
      }

      // div as optional title host (block) — only narrow looksLikeTitleHost hits.
      // Do NOT open blocks for title-page / heading wrappers (empty margin balloons).
      if (!tag.closing && ieq(tag.name, tag.nameLen, "div") &&
          (looksLikeTitleHost(tag) || classIsChapterLeftTitle(tag)) && !tag.selfClose) {
        closeBlock();
        // Alice .chapter is left + larger; other title hosts default center.
        const Align divAlign = [&]() {
          Align a = classIsChapterLeftTitle(tag) ? Align::Left : (styleSaysCenter(tag) ? Align::Center : Align::Left);
          Align htmlA = Align::Left;
          if (htmlAlignAttr(tag, htmlA)) a = htmlA;
          return a;
        }();
        openBlock(BlockKind::Paragraph, divAlign, kBlockNoIndent);
        out.setCurrentMarginsEmQ4(4, 8);
        RunStyle st = RunStyle::Regular;
        // 150% ≈ Plus1 on the user ladder (Literata 12 → 14).
        SizeStep sz = SizeStep::Plus1;
        if (classIsChapterLeftTitle(tag)) {
          st = RunStyle::Regular;  // Alice chapter title is not bold in CSS
        } else {
          st = RunStyle::Bold;
        }
        applyInlineStyle(tag, st, sz, nullptr);
        applyClassEmphasis(tag, st, sz);
        if (sz > SizeStep::Plus2) sz = SizeStep::Plus2;
        beginBlockStyle(st, sz);
        ++titleDivDepth;
        continue;
      }
      if (tag.closing && ieq(tag.name, tag.nameLen, "div") && titleDivDepth > 0) {
        closeBlock();
        endBlockStyle();
        --titleDivDepth;
        continue;
      }

      if (!tag.closing && ieq(tag.name, tag.nameLen, "br")) {
        // Inside headings: soft line break (Fourth Wing "CHAPTER<br/>ONE"), not a new left para.
        if (headingLevelOpen > 0) {
          flushText();
          if (inBlock) {
            out.endBlock();
            inBlock = false;
          }
          reopenHeadingIfNeeded();
          continue;
        }
        if (inBlock) {
          flushText();
          out.endBlock();
          inBlock = false;
        }
        openBlock(BlockKind::Paragraph, Align::Left, kBlockNoIndent);
        continue;
      }
      if (!tag.closing && ieq(tag.name, tag.nameLen, "hr")) {
        closeBlock();
        openBlock(BlockKind::HorizontalRule, Align::Left, kBlockNoIndent);
        closeBlock();
        // CSS: hr + p { text-indent: 0 }
        noIndentNextParagraph = true;
        continue;
      }

      // Images: store EPUB-relative href in the run text; dims filled later (probe).
      // Settings → Images (imageRendering): 0=Display, 1=Placeholder (alt text),
      // 2=Suppress (omit). Classic Section path used the same three modes.
      if (!tag.closing && (ieq(tag.name, tag.nameLen, "img") || ieq(tag.name, tag.nameLen, "image"))) {
        // Illuminae / Kindle dual assets: skip the display:none twin before any work.
        if (isSuppressedKindleTwinImg(tag)) {
          continue;
        }
        // Suppress: no plate, no alt, no spacing reservation.
        if (imageRendering == 2) {
          if (headingLevelOpen > 0) reopenHeadingIfNeeded();
          continue;
        }
        size_t vlen = 0;
        const char* src = attrValue(tag, "src", &vlen);
        if ((!src || vlen == 0)) {
          src = attrValue(tag, "href", &vlen);
        }
        if ((!src || vlen == 0)) {
          src = attrValue(tag, "xlink:href", &vlen);
        }
        size_t altLen = 0;
        const char* alt = attrValue(tag, "alt", &altLen);
        int side = classSaysFloatRight(tag) ? 2 : (classSaysFloatLeft(tag) ? 1 : floatInherit);
        Align imgHtml = Align::Left;
        if (htmlAlignAttr(tag, imgHtml)) {
          if (imgHtml == Align::Left) {
            side = 1;
          } else if (imgHtml == Align::Right) {
            side = 2;
          } else if (imgHtml == Align::Center) {
            side = 0;
          }
        }

        // Placeholder mode: classic "[Image: alt]" text only (no decode / no plate).
        if (imageRendering == 1) {
          closeBlock();
          std::string label;
          if (alt && altLen > 0) {
            std::string readable;
            if (buildReadableAltText(alt, altLen, readable, 1) && !readable.empty()) {
              label = "[Image: ";
              label += readable;
              label += "]";
            }
          }
          if (label.empty()) label = "[Image]";
          openBlock(BlockKind::Paragraph, Align::Center, kBlockNoIndent);
          out.setCurrentMarginsEmQ4(4, 4);
          (void)out.appendRun(RunStyle::Italic, SizeStep::Minus1, label.data(), label.size());
          closeBlock();
          if (headingLevelOpen > 0) reopenHeadingIfNeeded();
          continue;
        }

        // Prefer alt text for document captions (briefings, AAR headers, intercepts,
        // CLASSIFIED stamps). Memoranda body is usually real HTML; stamps/headers are imgs.
        if (alt && looksLikeDocumentAlt(alt, altLen)) {
          std::string readable;
          const size_t minOut = (altLen < 80) ? 6 : 24;  // short CLASSIFIED stamps OK
          if (buildReadableAltText(alt, altLen, readable, minOut)) {
            closeBlock();
            openBlock(BlockKind::Paragraph, Align::Left, kBlockNoIndent);
            out.setCurrentMarginsEmQ4(4, 6);
            const bool stamp = readable.size() < 40 || containsI(readable.data(), readable.size(), "CLASSIFIED");
            const auto style = stamp ? RunStyle::Bold : RunStyle::Italic;
            const auto size = stamp ? SizeStep::Body : SizeStep::Minus1;
            (void)out.appendRun(style, size, readable.data(), readable.size());
            closeBlock();
            if (headingLevelOpen > 0) reopenHeadingIfNeeded();
            continue;
          }
        }
        // Close current text block (heading may reopen after ornament).
        closeBlock();
        if (src && vlen > 0 && vlen < 400) {
          // Strip fragment (#...)
          size_t use = vlen;
          for (size_t i = 0; i < vlen; ++i) {
            if (src[i] == '#') {
              use = i;
              break;
            }
          }
          if (use > 0) {
            // Drop consecutive identical src (defensive if twin attrs missing).
            if (!out.blocks().empty()) {
              const Block& last = out.blocks().back();
              if (last.kind == BlockKind::Image && last.runCount > 0 && last.runBegin < out.runs().size()) {
                const Run& lr = out.runs()[last.runBegin];
                if (lr.textLen == use && out.textData() && std::memcmp(out.textData() + lr.textOff, src, use) == 0) {
                  if (headingLevelOpen > 0) reopenHeadingIfNeeded();
                  continue;
                }
              }
            }
            uint16_t imgFlags = kBlockNoIndent;
            // Parent .figleft / self float class → left letter float (Alice ornate C).
            // Illuminae: wrapper .figure_float_right_briefing → floatInherit right.
            // HTML align="left|right" on <img> is the same contract as classic.
            if (side == 1) imgFlags = static_cast<uint16_t>(imgFlags | kBlockFloatLeft);
            if (side == 2) imgFlags = static_cast<uint16_t>(imgFlags | kBlockFloatRight);
            // Fourth Wing: .orn img { width: 12% } — small centered chapter ornament.
            // Only class / wrapper / explicit orn.png basename — do not match any
            // path containing "/orn" (false-positive on "ornate", "morning", folders).
            const bool lastWasHeading = !out.blocks().empty() && out.blocks().back().kind >= BlockKind::Heading1 &&
                                        out.blocks().back().kind <= BlockKind::Heading6;
            const bool isOrnament = ornamentDepth > 0 || attrHasClass(tag, "orn") || attrHasClass(tag, "ornament") ||
                                    (use >= 7 && containsI(src, use, "orn.png")) || containsI(src, use, "ornament") ||
                                    containsI(src, use, "flourish") || containsI(src, use, "fleuron") ||
                                    containsI(src, use, "headpiece");
            if (isOrnament) imgFlags = static_cast<uint16_t>(imgFlags | kBlockOrnament);
            openBlock(BlockKind::Image, Align::Center, imgFlags);
            // Chapter-open decoration: air between title and flourish (v0.1.8 look).
            if (side == 0 && (lastWasHeading || headingLevelOpen > 0 || isOrnament)) {
              out.setCurrentMarginsEmQ4(6, 10);
            }
            int cssW = parseStyleWidthPx(tag);
            if (cssW <= 0) cssW = floatWidthPx;
            // Side floats without rich alt (email chrome icons, etc.).
            if (cssW <= 0 && side != 0) cssW = 160;
            // Ornament: provisional ~12% of a 400px content width until prepare uses real viewport.
            if (isOrnament && cssW <= 0) cssW = 48;
            if (cssW > 0 && !out.blocksMutable().empty()) {
              auto& b = out.blocksMutable().back();
              b.imageW = static_cast<uint16_t>(std::min(2000, cssW));
              b.imageH = b.imageW;
            }
            (void)out.appendRun(RunStyle::Regular, SizeStep::Body, src, use);
            closeBlock();
            // Resume multi-line heading text after ornament (CHAPTER / ONE).
            if (headingLevelOpen > 0) reopenHeadingIfNeeded();
          }
        } else if (alt && altLen >= 12) {
          // No src / unsupported: classic fallback — show alt as text when useful.
          std::string readable;
          if (buildReadableAltText(alt, altLen, readable, looksLikeDocumentAlt(alt, altLen) ? 6 : 24)) {
            openBlock(BlockKind::Paragraph, Align::Left, kBlockNoIndent);
            (void)out.appendRun(RunStyle::Italic, SizeStep::Minus1, readable.data(), readable.size());
            closeBlock();
          }
          if (headingLevelOpen > 0) reopenHeadingIfNeeded();
        }
        continue;
      }
      // Inline SVG: no raster pipeline — skip (was [image] tofu).
      if (!tag.closing && ieq(tag.name, tag.nameLen, "svg")) {
        continue;
      }

      if (!tag.closing && (ieq(tag.name, tag.nameLen, "b") || ieq(tag.name, tag.nameLen, "strong"))) {
        flushText();
        if (!tag.selfClose && innerIsOnlyOpeningPunct(p, end, tag.name, tag.nameLen)) {
          // Keep a frame so the matching </b> pop is balanced, but do not bold "[".
          pushStyle(curStyle().style, curStyle().size);
        } else {
          pushStyle(mergeBold(curStyle().style), curStyle().size);
        }
        continue;
      }
      if (tag.closing && (ieq(tag.name, tag.nameLen, "b") || ieq(tag.name, tag.nameLen, "strong"))) {
        flushText();
        popStyle();
        continue;
      }
      if (!tag.closing &&
          (ieq(tag.name, tag.nameLen, "i") || ieq(tag.name, tag.nameLen, "em") || ieq(tag.name, tag.nameLen, "cite"))) {
        flushText();
        pushStyle(mergeItalic(curStyle().style), curStyle().size);
        continue;
      }
      if (tag.closing &&
          (ieq(tag.name, tag.nameLen, "i") || ieq(tag.name, tag.nameLen, "em") || ieq(tag.name, tag.nameLen, "cite"))) {
        flushText();
        popStyle();
        continue;
      }

      // Text decorations and scripts. These were dropped entirely before v20:
      // <sup> footnote markers rendered full-size on the baseline, and <u>/<s>/
      // <del>/<ins> passages lost the very thing that gave them meaning.
      // GfxRenderer::drawText already understands the SUP/SUB bits (it scales the
      // glyph and halves the advance); underline/strikethrough are painted as
      // lines by RivuletEngine::paint, matching the classic TextBlock::render.
      {
        RunStyle decoration = RunStyle::Regular;
        if (ieq(tag.name, tag.nameLen, "sup")) {
          decoration = RunStyle::Superscript;
        } else if (ieq(tag.name, tag.nameLen, "sub")) {
          decoration = RunStyle::Subscript;
        } else if (ieq(tag.name, tag.nameLen, "u") || ieq(tag.name, tag.nameLen, "ins")) {
          decoration = RunStyle::Underline;
        } else if (ieq(tag.name, tag.nameLen, "s") || ieq(tag.name, tag.nameLen, "strike") ||
                   ieq(tag.name, tag.nameLen, "del")) {
          decoration = RunStyle::Strikethrough;
        }
        if (decoration != RunStyle::Regular) {
          flushText();
          if (tag.closing) {
            popStyle();
          } else if (!tag.selfClose) {
            pushStyle(mergeDecoration(curStyle().style, decoration), curStyle().size);
          }
          continue;
        }
      }

      // span / font: inherit + class/style emphasis
      if (!tag.closing && (ieq(tag.name, tag.nameLen, "span") || ieq(tag.name, tag.nameLen, "font")) &&
          !tag.selfClose) {
        flushText();
        RunStyle st = curStyle().style;
        SizeStep sz = curStyle().size;
        if (!innerIsOnlyOpeningPunct(p, end, tag.name, tag.nameLen)) {
          applyInlineStyle(tag, st, sz);
          applyClassEmphasis(tag, st, sz);
        }
        pushStyle(st, sz);
        continue;
      }
      if (tag.closing && (ieq(tag.name, tag.nameLen, "span") || ieq(tag.name, tag.nameLen, "font"))) {
        flushText();
        popStyle();
        continue;
      }

      continue;
    }

    // tableDepth > 1 = inside a nested table, whose content the classic engine
    // discarded too. Suppressing only its tags would still let its text leak into
    // the surrounding block.
    if (inSkip || inHidden || tableDepth > 1) {
      ++p;
      continue;
    }
    if (!inBlock) {
      uint16_t f = 0;
      if (dropCapArmed) f = static_cast<uint16_t>(f | kBlockDropCap | kBlockNoIndent);
      if (unindentInherit) f = static_cast<uint16_t>(f | kBlockNoIndent);
      openBlock(BlockKind::Paragraph, Align::Justify, f);
      out.setCurrentMarginsEmQ4(0, 4);
      if (dropCapArmed) {
        out.markDropCapOnCurrent();
        dropCapArmed = false;
      }
    }
    if (*p == '&') {
      bool oom = false;
      const size_t n = decodeEntity(p, end, textAcc, &oom);
      if (oom) {
        out.markFailed();
        break;
      }
      p += n ? n : 1;
      // Keep textAcc bounded so push never aborts.
      if (textAcc.size() > 1536) flushText();
      continue;
    }
    const char* t0 = p;
    while (p < end && *p != '<' && *p != '&') ++p;
    // Chunk long text runs; flush before textAcc needs a large reallocation.
    size_t remain = static_cast<size_t>(p - t0);
    const char* chunk = t0;
    // After </i>/</b>/<span>, HTML whitespace is often the ONLY separator before
    // the next word. appendCollapsedText drops leading spaces when dst is empty,
    // which glued "Vampire"+"skill". If this block already has runs, keep one
    // leading space when the chunk starts with whitespace.
    if (remain > 0 && textAcc.empty() && inBlock && !out.blocks().empty() && out.blocks().back().runCount > 0) {
      const char c0 = *chunk;
      if (c0 == ' ' || c0 == '\n' || c0 == '\t' || c0 == '\r') {
        if (!safePushChar(textAcc, ' ')) {
          out.markFailed();
          break;
        }
      }
    }
    while (remain > 0 && !out.failed()) {
      if (textAcc.size() > 1536) flushText();
      const size_t take = std::min(remain, size_t(400));
      if (!appendCollapsedText(textAcc, chunk, take, false)) {
        out.markFailed();
        break;
      }
      chunk += take;
      remain -= take;
    }
    if (out.failed()) break;
  }

  closeBlock();
  // Partial chapter after OOM is still usable if we have any blocks.
  if (out.failed() && !out.empty()) {
    // leave failed_ set so caller can log; still return true for partial IR
  }
  return !out.empty();
}

}  // namespace rivulet
