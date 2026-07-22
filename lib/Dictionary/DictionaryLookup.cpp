#include "DictionaryLookup.h"

#include <Esp.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kMagic[4] = {'C', 'X', 'D', '1'};

#pragma pack(push, 1)
struct DictHeader {
  char magic[4];
  uint32_t wordCount;
  uint32_t indexOffset;
  uint32_t dataOffset;
};
struct IndexEntry {
  char key[DictionaryLookup::kMaxKeyLen];
  uint32_t offset;
  uint32_t length;
};
#pragma pack(pop)

static_assert(sizeof(DictHeader) == 16, "DictHeader size");
static_assert(sizeof(IndexEntry) == DictionaryLookup::kMaxKeyLen + 8, "IndexEntry size");

struct Pack {
  const char* path;
  const char* label;
};

// Search order for Auto: monolingual first, then bilingual.
constexpr Pack kPacks[] = {
    {DictionaryLookup::kPathEn, "EN"},
    {DictionaryLookup::kPathEs, "ES"},
    {DictionaryLookup::kPathEsEn, "ES->EN"},
    {DictionaryLookup::kPathEnEs, "EN->ES"},
};

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool readExact(HalFile& file, void* buf, const size_t n) {
  auto* out = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    const int r = file.read(out + got, n - got);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

bool readHeader(HalFile& file, DictHeader& hdr) {
  if (!file.seekSet(0)) return false;
  uint8_t raw[sizeof(DictHeader)];
  if (!readExact(file, raw, sizeof(raw))) return false;
  memcpy(hdr.magic, raw, 4);
  hdr.wordCount = readLe32(raw + 4);
  hdr.indexOffset = readLe32(raw + 8);
  hdr.dataOffset = readLe32(raw + 12);
  if (memcmp(hdr.magic, kMagic, 4) != 0) return false;
  if (hdr.wordCount == 0 || hdr.wordCount > 2000000u) return false;
  if (hdr.indexOffset < sizeof(DictHeader)) return false;
  if (hdr.dataOffset < hdr.indexOffset) return false;
  return true;
}

bool readIndexEntry(HalFile& file, const DictHeader& hdr, const uint32_t i, IndexEntry& entry) {
  const uint32_t pos = hdr.indexOffset + i * static_cast<uint32_t>(sizeof(IndexEntry));
  if (!file.seekSet(pos)) return false;
  return readExact(file, &entry, sizeof(entry));
}

int keyCompare(const char* needle, const char* entryKey) {
  for (size_t i = 0; i < DictionaryLookup::kMaxKeyLen; ++i) {
    const unsigned char a = static_cast<unsigned char>(needle[i]);
    const unsigned char b = static_cast<unsigned char>(entryKey[i]);
    if (a != b) return static_cast<int>(a) - static_cast<int>(b);
    if (a == 0) return 0;
  }
  return 0;
}

// Spanish object clitics longest-first (ayudame -> ayuda, damelo -> dame).
constexpr const char* kEsClitics[] = {
    "melos", "melas", "telos", "telas", "selos", "selas", "noslos", "noslas", "noslo", "nosla",
    "selo",  "sela",  "melo",  "mela",  "telo",  "tela",  "lelo",   "lela",
    "les",   "los",   "las",   "nos",   "os",    "me",    "te",     "se",    "lo",    "la",    "le",
};

// Human-readable clitic gloss for the e-ink popup (English, short).
const char* esCliticGloss(const char* clitic) {
  if (!clitic) return "";
  if (strcmp(clitic, "me") == 0) return "me / to me";
  if (strcmp(clitic, "te") == 0) return "you / to you";
  if (strcmp(clitic, "se") == 0) return "oneself / him/her";
  if (strcmp(clitic, "lo") == 0 || strcmp(clitic, "la") == 0) return "it / him/her";
  if (strcmp(clitic, "le") == 0) return "to him/her";
  if (strcmp(clitic, "les") == 0) return "to them";
  if (strcmp(clitic, "los") == 0 || strcmp(clitic, "las") == 0) return "them";
  if (strcmp(clitic, "nos") == 0) return "us";
  if (strcmp(clitic, "os") == 0) return "you (pl.)";
  if (strncmp(clitic, "me", 2) == 0) return "me + object";
  if (strncmp(clitic, "te", 2) == 0) return "you + object";
  if (strncmp(clitic, "se", 2) == 0) return "se + object";
  return "pronoun object";
}

// Short English object pronoun for composing natural phrases ("help me").
// Returns nullptr for multi-clitic strings (me+lo) where a single object is unclear.
const char* esCliticEnglishObject(const char* clitic) {
  if (!clitic || !clitic[0] || strchr(clitic, '+') != nullptr) return nullptr;
  if (strcmp(clitic, "me") == 0) return "me";
  if (strcmp(clitic, "te") == 0) return "you";
  if (strcmp(clitic, "se") == 0) return "oneself";
  if (strcmp(clitic, "lo") == 0 || strcmp(clitic, "la") == 0) return "it";
  if (strcmp(clitic, "le") == 0) return "him/her";
  if (strcmp(clitic, "les") == 0) return "them";
  if (strcmp(clitic, "los") == 0 || strcmp(clitic, "las") == 0) return "them";
  if (strcmp(clitic, "nos") == 0) return "us";
  if (strcmp(clitic, "os") == 0) return "you";
  return nullptr;
}

// Pull the first short English gloss from a bilingual def for phrases like "help me".
// Handles "to help", "help, aid, assistance", multi-line POS/IPA layouts.
bool extractFirstEnglishGloss(const char* def, char* out, size_t outLen) {
  if (!def || !out || outLen < 4) return false;
  out[0] = '\0';

  const char* p = def;
  // Prefer the first contentful line; skip IPA (/.../) and bare POS tags.
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') ++p;
    if (!*p) break;
    const char* lineEnd = p;
    while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
    const size_t lineLen = static_cast<size_t>(lineEnd - p);
    // Skip pronunciation lines.
    if (*p == '/') {
      p = lineEnd;
      continue;
    }
    // Skip short part-of-speech tags: noun, verb, adj, ...
    if (lineLen <= 12) {
      bool allAlpha = true;
      for (size_t i = 0; i < lineLen; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '-')) {
          allAlpha = false;
          break;
        }
      }
      if (allAlpha) {
        // Known short POS-ish words only — don't skip real one-word glosses later.
        char tag[16];
        const size_t n = lineLen < sizeof(tag) - 1 ? lineLen : sizeof(tag) - 1;
        memcpy(tag, p, n);
        tag[n] = '\0';
        for (char* t = tag; *t; ++t) {
          if (*t >= 'A' && *t <= 'Z') *t = static_cast<char>(*t - 'A' + 'a');
        }
        if (strcmp(tag, "noun") == 0 || strcmp(tag, "verb") == 0 || strcmp(tag, "adj") == 0 ||
            strcmp(tag, "adjective") == 0 || strcmp(tag, "adverb") == 0 || strcmp(tag, "adv") == 0 ||
            strcmp(tag, "prep") == 0 || strcmp(tag, "pronoun") == 0 || strcmp(tag, "interj") == 0 ||
            strcmp(tag, "article") == 0 || strcmp(tag, "conj") == 0) {
          p = lineEnd;
          continue;
        }
      }
    }
    break;
  }
  if (!*p) return false;

  // Copy until comma / semicolon / period / paren / newline / pipe.
  size_t w = 0;
  while (p[w] && w + 1 < outLen && w < 40) {
    const char c = p[w];
    if (c == ',' || c == ';' || c == '.' || c == '(' || c == '[' || c == '|' || c == '\n' || c == '\r' ||
        c == '/') {
      break;
    }
    out[w] = c;
    ++w;
  }
  out[w] = '\0';
  // Trim trailing spaces.
  while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) out[--w] = '\0';
  if (w < 2) {
    out[0] = '\0';
    return false;
  }

  // "to help" → "help" (infinitive glosses from ES→EN packs).
  if (w > 3 && out[0] == 't' && out[1] == 'o' && out[2] == ' ') {
    memmove(out, out + 3, w - 2);  // includes NUL
    w -= 3;
  }
  // Drop leading article for cleaner phrases.
  if (w > 2 && out[0] == 'a' && out[1] == ' ') {
    memmove(out, out + 2, w - 1);
    w -= 2;
  } else if (w > 4 && out[0] == 'a' && out[1] == 'n' && out[2] == ' ' ) {
    memmove(out, out + 3, w - 2);
    w -= 3;
  } else if (w > 4 && out[0] == 't' && out[1] == 'h' && out[2] == 'e' && out[3] == ' ') {
    memmove(out, out + 4, w - 3);
    w -= 4;
  }
  // Keep phrases short: at most 3 words.
  int spaces = 0;
  for (size_t i = 0; out[i]; ++i) {
    if (out[i] == ' ') {
      ++spaces;
      if (spaces >= 3) {
        out[i] = '\0';
        break;
      }
    }
  }
  return out[0] != '\0' && strlen(out) >= 2;
}

// Build "help me" style first line when clitic + bilingual gloss allow it.
bool composeCliticEnglishPhrase(const char* clitic, const char* def, char* out, size_t outLen) {
  if (!out || outLen < 8) return false;
  out[0] = '\0';
  const char* obj = esCliticEnglishObject(clitic);
  if (!obj) return false;
  char gloss[48];
  if (!extractFirstEnglishGloss(def, gloss, sizeof(gloss))) return false;
  // Avoid nonsense like "helper me" from pure agent nouns when gloss ends with -er/-or
  // and is a single token — still allow "help" (4 letters, ends with p).
  // Only skip when the gloss is clearly multi-syllable agentive and longer than 6.
  // Prefer verb-ish short heads: always compose for common object clitics.
  snprintf(out, outLen, "%s %s", gloss, obj);
  return true;
}

bool trySpanishCliticStrip(char* word, char* strippedClitic, size_t cliticCap) {
  const size_t n = strlen(word);
  if (n < 5) return false;
  if (strippedClitic && cliticCap) strippedClitic[0] = '\0';

  for (const char* c : kEsClitics) {
    const size_t sn = strlen(c);
    if (n <= sn + 2) continue;
    if (strcmp(word + (n - sn), c) != 0) continue;
    if (strippedClitic && cliticCap > 1) {
      strncpy(strippedClitic, c, cliticCap - 1);
      strippedClitic[cliticCap - 1] = '\0';
    }
    word[n - sn] = '\0';
    return true;
  }
  return false;
}

bool tryStemVariants(char* word) {
  const size_t n = strlen(word);
  if (n < 4) return false;

  auto endsWith = [word, n](const char* suf) {
    const size_t sn = strlen(suf);
    return n > sn && strcmp(word + (n - sn), suf) == 0;
  };
  auto stripSuffix = [word, n](const size_t sn) {
    if (n <= sn + 2) return false;  // keep a usable stem
    word[n - sn] = '\0';
    return true;
  };

  // Spanish clitics before English stems so "ayudame" is not mangled.
  char discarded[12];
  if (trySpanishCliticStrip(word, discarded, sizeof(discarded))) {
    return true;
  }

  // English-ish. Longer suffixes first so -ly is not lost under -s/-es.
  // Adverbs: obsequiously → obsequious; happily → happy; probably → probable.
  if (endsWith("ily") && n > 5) {
    word[n - 3] = 'y';
    word[n - 2] = '\0';
    return true;
  }
  if ((endsWith("ably") || endsWith("ibly")) && n > 6) {
    // probably → probable, possibly → possible
    word[n - 1] = 'e';
    return true;
  }
  if (endsWith("ly") && n > 5) {
    word[n - 2] = '\0';
    return true;
  }
  if (endsWith("ies") && n > 4) {
    word[n - 3] = 'y';
    word[n - 2] = '\0';
    return true;
  }
  if (endsWith("ing") && n > 5) {
    word[n - 3] = '\0';
    return true;
  }
  if (endsWith("ed") && n > 4) {
    word[n - 2] = '\0';
    return true;
  }
  if (endsWith("es") && n > 4) {
    word[n - 2] = '\0';
    return true;
  }
  // Spanish/English plural -s (after -ly so adverbs are not mangled).
  if (endsWith("s") && n > 3 && word[n - 2] != 's') {
    word[n - 1] = '\0';
    return true;
  }
  return false;
}

// Fold common Spanish accents for a second-chance lookup (á→a, ñ stays).
bool foldAccentsInPlace(char* s) {
  bool changed = false;
  char* w = s;
  for (const unsigned char* r = reinterpret_cast<const unsigned char*>(s); *r;) {
    if (r[0] == 0xC3 && r[1] != 0) {
      unsigned char c2 = r[1];
      char repl = 0;
      if (c2 == 0xA1 || c2 == 0x81)
        repl = 'a';
      else if (c2 == 0xA9 || c2 == 0x89)
        repl = 'e';
      else if (c2 == 0xAD || c2 == 0x8D)
        repl = 'i';
      else if (c2 == 0xB3 || c2 == 0x93)
        repl = 'o';
      else if (c2 == 0xBA || c2 == 0x9A || c2 == 0xBC || c2 == 0x9C)
        repl = 'u';
      if (repl) {
        *w++ = repl;
        r += 2;
        changed = true;
        continue;
      }
    }
    if ((*r & 0x80) == 0) {
      *w++ = static_cast<char>(*r++);
    } else if ((*r & 0xE0) == 0xC0 && r[1]) {
      *w++ = static_cast<char>(*r++);
      *w++ = static_cast<char>(*r++);
    } else if ((*r & 0xF0) == 0xE0 && r[1] && r[2]) {
      *w++ = static_cast<char>(*r++);
      *w++ = static_cast<char>(*r++);
      *w++ = static_cast<char>(*r++);
    } else {
      ++r;
    }
  }
  *w = '\0';
  return changed;
}

bool binarySearch(HalFile& file, const DictHeader& hdr, const char* key, IndexEntry& found) {
  int lo = 0;
  int hi = static_cast<int>(hdr.wordCount) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    IndexEntry entry{};
    if (!readIndexEntry(file, hdr, static_cast<uint32_t>(mid), entry)) return false;
    entry.key[DictionaryLookup::kMaxKeyLen - 1] = '\0';
    const int cmp = keyCompare(key, entry.key);
    if (cmp == 0) {
      found = entry;
      return true;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

// UI fonts lack IPA / fancy punctuation. Fold to ASCII + Spanish accents so the
// e-ink popup does not show tofu (black diamond) glyphs.
//
// Important: most IPA lives in U+0250–U+02AF and encodes as *2-byte* UTF-8
// (not 3-byte). Dropping those left broken leftovers like "/tdd/" from "/ðɪs/".
void appendAscii(char* out, size_t& w, const size_t cap, const char* ascii) {
  while (*ascii && w + 1 < cap) {
    out[w++] = *ascii++;
  }
}

// Map one Unicode code point → ASCII (or keep Spanish). Returns false to skip.
bool foldCodepoint(const uint32_t cp, char* out, size_t& w, const size_t cap) {
  auto put = [&](char c) {
    if (w + 1 < cap) out[w++] = c;
  };
  auto putStr = [&](const char* s) { appendAscii(out, w, cap, s); };

  if (cp < 0x80) {
    put(static_cast<char>(cp));
    return true;
  }

  // Keep Spanish accents as UTF-8 (UI fonts usually include these).
  auto putUtf8Spanish = [&](unsigned char b1) {
    if (w + 2 < cap) {
      out[w++] = static_cast<char>(0xC3);
      out[w++] = static_cast<char>(b1);
    }
  };
  switch (cp) {
    case 0x00E1:
      putUtf8Spanish(0xA1);
      return true;  // á
    case 0x00E9:
      putUtf8Spanish(0xA9);
      return true;  // é
    case 0x00ED:
      putUtf8Spanish(0xAD);
      return true;  // í
    case 0x00F3:
      putUtf8Spanish(0xB3);
      return true;  // ó
    case 0x00FA:
      putUtf8Spanish(0xBA);
      return true;  // ú
    case 0x00FC:
      putUtf8Spanish(0xBC);
      return true;  // ü
    case 0x00F1:
      putUtf8Spanish(0xB1);
      return true;  // ñ
    case 0x00C1:
      putUtf8Spanish(0x81);
      return true;  // Á
    case 0x00C9:
      putUtf8Spanish(0x89);
      return true;  // É
    case 0x00CD:
      putUtf8Spanish(0x8D);
      return true;  // Í
    case 0x00D3:
      putUtf8Spanish(0x93);
      return true;  // Ó
    case 0x00DA:
      putUtf8Spanish(0x9A);
      return true;  // Ú
    case 0x00DC:
      putUtf8Spanish(0x9C);
      return true;  // Ü
    case 0x00D1:
      putUtf8Spanish(0x91);
      return true;  // Ñ
    default:
      break;
  }

  // Latin-1 / common
  if (cp == 0x00A0) {
    put(' ');
    return true;
  }
  if (cp == 0x00E6 || cp == 0x00C6) {
    putStr("ae");
    return true;
  }
  if (cp == 0x00F0 || cp == 0x00D0) {
    putStr("th");
    return true;
  }  // ð Ð
  if (cp == 0x00F8 || cp == 0x00D8) {
    put('o');
    return true;
  }

  // Curly quotes / dashes / ellipsis
  if (cp == 0x2018 || cp == 0x2019 || cp == 0x02BC || cp == 0x02B9) {
    put('\'');
    return true;
  }
  if (cp == 0x201C || cp == 0x201D) {
    put('"');
    return true;
  }
  if (cp == 0x2013 || cp == 0x2014) {
    put('-');
    return true;
  }
  if (cp == 0x2026) {
    putStr("...");
    return true;
  }
  if (cp == 0x2022 || cp == 0x00B7) {
    put('-');
    return true;
  }

  // IPA stress / length (often 2-byte UTF-8)
  if (cp == 0x02C8) {
    put('\'');
    return true;
  }  // ˈ
  if (cp == 0x02CC) {
    put(',');
    return true;
  }  // ˌ
  if (cp == 0x02D0 || cp == 0x02D1) {
    put(':');
    return true;
  }  // ː

  // IPA letters (U+0250–U+02AF mostly 2-byte UTF-8)
  switch (cp) {
    case 0x0259:
    case 0x025A:
    case 0x025C:
    case 0x025D:
      putStr("uh");
      return true;  // ə ɚ ɜ ɝ
    case 0x026A:
      put('i');
      return true;  // ɪ
    case 0x028A:
      put('u');
      return true;  // ʊ
    case 0x028C:
      put('u');
      return true;  // ʌ
    case 0x0251:
    case 0x0252:
      put('a');
      return true;  // ɑ ɒ
    case 0x0254:
      put('o');
      return true;  // ɔ
    case 0x025B:
      put('e');
      return true;  // ɛ
    case 0x0283:
      putStr("sh");
      return true;  // ʃ
    case 0x0292:
      putStr("zh");
      return true;  // ʒ
    case 0x0279:
    case 0x027B:
    case 0x027E:
      put('r');
      return true;  // ɹ
    case 0x0261:
      put('g');
      return true;  // ɡ
    case 0x014B:
      putStr("ng");
      return true;  // ŋ
    case 0x03B8:
      putStr("th");
      return true;  // θ
    case 0x026C:
      putStr("l");
      return true;
    case 0x0272:
      put('n');
      return true;
    case 0x0281:
      put('R');
      return true;
    case 0x0294:
      put('\'');
      return true;  // ʔ
    case 0x00E6:
      putStr("ae");
      return true;
    default:
      break;
  }

  // Combining marks / modifiers / super-sub — drop (no tofu)
  if (cp >= 0x0300 && cp <= 0x036F) return true;
  if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;
  if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;
  if (cp >= 0x02B0 && cp <= 0x02FF) return true;  // modifier letters
  if (cp >= 0x2070 && cp <= 0x209F) return true;
  return true;  // unknown → drop
}

void foldForUiFontInPlace(char* text, const size_t cap) {
  if (!text || cap == 0) return;

  char out[DictionaryLookup::kMaxDefinitionLen + 1];
  size_t w = 0;
  const unsigned char* r = reinterpret_cast<const unsigned char*>(text);

  while (*r && w + 4 < sizeof(out)) {
    uint32_t cp = 0;
    if (*r < 0x80) {
      cp = *r++;
    } else if ((*r & 0xE0) == 0xC0 && r[1]) {
      cp = (static_cast<uint32_t>(r[0] & 0x1F) << 6) | static_cast<uint32_t>(r[1] & 0x3F);
      r += 2;
    } else if ((*r & 0xF0) == 0xE0 && r[1] && r[2]) {
      cp = (static_cast<uint32_t>(r[0] & 0x0F) << 12) | (static_cast<uint32_t>(r[1] & 0x3F) << 6) |
           static_cast<uint32_t>(r[2] & 0x3F);
      r += 3;
    } else if ((*r & 0xF8) == 0xF0 && r[1] && r[2] && r[3]) {
      r += 4;  // emoji / rare — skip
      continue;
    } else {
      ++r;
      continue;
    }
    foldCodepoint(cp, out, w, sizeof(out));
  }
  out[w] = '\0';

  // Drop mangled pronunciation lines like "/td/" (almost no letters between slashes).
  char finalBuf[DictionaryLookup::kMaxDefinitionLen + 1];
  size_t f = 0;
  const char* line = out;
  bool wroteAny = false;
  while (*line && f + 1 < sizeof(finalBuf)) {
    const char* nl = strchr(line, '\n');
    const size_t len = nl ? static_cast<size_t>(nl - line) : strlen(line);
    bool drop = false;
    if (len >= 2 && line[0] == '/' && line[len - 1] == '/') {
      int letters = 0;
      for (size_t i = 1; i + 1 < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(line[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) letters++;
      }
      if (letters < 2) drop = true;
    }
    if (!drop) {
      if (wroteAny && f + 1 < sizeof(finalBuf)) finalBuf[f++] = '\n';
      for (size_t i = 0; i < len && f + 1 < sizeof(finalBuf); ++i) finalBuf[f++] = line[i];
      wroteAny = true;
    }
    if (!nl) break;
    line = nl + 1;
  }
  finalBuf[f] = '\0';

  const char* src = wroteAny ? finalBuf : out;
  const size_t n = std::min(strlen(src), cap > 0 ? cap - 1 : 0);
  memcpy(text, src, n);
  text[n] = '\0';
}

bool loadDefinition(HalFile& file, const DictHeader& hdr, const IndexEntry& entry, char* outDef, const size_t outDefLen) {
  if (outDef == nullptr || outDefLen == 0) return false;
  outDef[0] = '\0';
  if (entry.length == 0) return false;
  if (entry.length > DictionaryLookup::kMaxDefinitionLen) {
    LOG_ERR("DICT", "Definition too long (%u)", static_cast<unsigned>(entry.length));
    return false;
  }
  const uint32_t absOff = hdr.dataOffset + entry.offset;
  if (!file.seekSet(absOff)) return false;
  const size_t copyLen = std::min(static_cast<size_t>(entry.length), outDefLen - 1);
  if (!readExact(file, outDef, copyLen)) return false;
  outDef[copyLen] = '\0';

  // Strip simple HTML tags if present.
  char* w = outDef;
  for (char* r = outDef; *r; ++r) {
    if (*r == '<') {
      while (*r && *r != '>') ++r;
      if (!*r) break;
      continue;
    }
    *w++ = *r;
  }
  *w = '\0';

  // Fold IPA / fancy punctuation to glyphs the UI font can draw.
  foldForUiFontInPlace(outDef, outDefLen);

  // Strip WordNet-style synonym tails ("| Syn: a, b, c"). They often come from
  // the wrong sense and read as noise on a small popup (e.g. charged →
  // "mission, tutelage" from charge/duty).
  char* syn = strstr(outDef, "| Syn:");
  if (!syn) syn = strstr(outDef, "| syn:");
  if (syn) {
    while (syn > outDef && (syn[-1] == ' ' || syn[-1] == '\n' || syn[-1] == '.')) --syn;
    *syn = '\0';
  }

  return outDef[0] != '\0';
}

// Reformat a pack body for a small e-ink popup.
//
// Multi-POS dumps (e.g. "one" with numeral/pronoun/noun/adj/…) become:
//   numeral
//   • the number represented by 1 …
//   pronoun
//   • one of a group …
// not a wall of (wn)/POS lines. Caps: 2 POS sections, 2 senses each.
// Pack headers ("@EN") are added later by lookupAuto.
//
// Bullet: UTF-8 U+2022 "•"
void formatDefBodyForUi(char* def, const size_t cap) {
  if (!def || cap < 8 || !def[0]) return;

  constexpr int kMaxPosBlocks = 2;
  constexpr int kMaxSensesPerPos = 2;
  constexpr int kMaxSensesTotal = 4;
  constexpr char kBullet[] = "\xE2\x80\xA2 ";

  // Bilingual dumps (e.g. Spanish "a" in es-en) often use " / " on one long line.
  // Normalize to newlines first so the POS/sense logic can cap them.
  if (strchr(def, '\n') == nullptr && strstr(def, " / ") != nullptr) {
    char split[DictionaryLookup::kMaxDefinitionLen + 1];
    size_t o = 0;
    const char* r = def;
    while (*r && o + 2 < sizeof(split)) {
      if (r[0] == ' ' && r[1] == '/' && r[2] == ' ') {
        split[o++] = '\n';
        r += 3;
        while (*r == ' ') ++r;
        continue;
      }
      split[o++] = *r++;
    }
    split[o] = '\0';
    strncpy(def, split, cap - 1);
    def[cap - 1] = '\0';
  }

  char out[DictionaryLookup::kMaxDefinitionLen + 1];
  size_t w = 0;
  int totalSenses = 0;
  int posBlocks = 0;
  int sensesThisPos = 0;
  bool havePosOpen = false;
  char lastPron[48] = {};

  auto emitLine = [&](const char* line) {
    if (!line || !line[0] || w + 2 >= sizeof(out)) return;
    if (w > 0 && w + 1 < sizeof(out)) out[w++] = '\n';
    while (*line && w + 1 < sizeof(out)) out[w++] = *line++;
    out[w] = '\0';
  };

  auto isOfCounter = [](const char* s) -> bool {
    if (!s || !std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    const char* p = s;
    while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
    return p[0] == ' ' && p[1] == 'o' && p[2] == 'f' && p[3] == ' ' &&
           std::isdigit(static_cast<unsigned char>(p[4]));
  };

  auto isSourceTag = [](const char* s) -> bool {
    // "(wn)", "(WN)", "[wn]", "wn" — pack provenance, not reader content
    if (!s || !s[0]) return false;
    if ((s[0] == '(' || s[0] == '[') && s[1]) {
      char tag[16];
      size_t n = 0;
      for (const char* p = s + 1; *p && *p != ')' && *p != ']' && n + 1 < sizeof(tag); ++p) {
        tag[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
      }
      tag[n] = '\0';
      return strcmp(tag, "wn") == 0 || strcmp(tag, "oewn") == 0 || strcmp(tag, "webster") == 0 ||
             strcmp(tag, "wiktionary") == 0 || strcmp(tag, "wt") == 0;
    }
    return false;
  };

  auto isPosLine = [](const char* s) -> bool {
    if (!s || !s[0]) return false;
    // Known short POS labels only (avoid eating real one-word glosses).
    static const char* kPos[] = {
        "noun",       "verb",      "adjective",  "adverb",     "pronoun",    "numeral",
        "determiner", "article",   "preposition","conjunction","interjection","particle",
        "prefix",     "suffix",    "phrase",     "abbreviation","adj",        "adv",
        "prep",       "conj",      "pron",       "det",        "number",
    };
    char lower[24];
    size_t n = 0;
    for (const char* p = s; *p && n + 1 < sizeof(lower); ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c >= 'A' && c <= 'Z')
        lower[n++] = static_cast<char>(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || c == ' ' || c == '-')
        lower[n++] = static_cast<char>(c);
      else
        return false;
    }
    lower[n] = '\0';
    if (n < 2 || n > 14) return false;
    for (const char* p : kPos) {
      if (strcmp(lower, p) == 0) return true;
    }
    return false;
  };

  auto isPronLine = [&](const char* s) -> bool {
    return s && s[0] && (s[0] == '/' || (s[0] == '(' && !isSourceTag(s)));
  };

  auto shortenSense = [](char* sense) {
    // Prefer the first clause when a single sense jammed two glosses with "; ".
    // "one thing (among…); one member of a group" → keep first clause only.
    if (!sense) return;
    char* semi = strstr(sense, "; ");
    if (!semi) return;
    const size_t firstLen = static_cast<size_t>(semi - sense);
    if (firstLen < 12 || firstLen > 90) return;
    // Keep semicolon join when first clause is tiny ("a; any") — drop only long doubles.
    if (firstLen >= 18) *semi = '\0';
  };

  auto emitSense = [&](const char* sense) {
    if (!sense || totalSenses >= kMaxSensesTotal) return;
    if (havePosOpen && sensesThisPos >= kMaxSensesPerPos) return;
    while (*sense == ' ' || *sense == '\t' || *sense == ':') ++sense;
    if (!*sense) return;
    char buf[DictionaryLookup::kMaxDefinitionLen + 1];
    strncpy(buf, sense, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Trim trailing spaces/junk
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == ';' || buf[n - 1] == '.')) buf[--n] = '\0';
    shortenSense(buf);
    if (!buf[0]) return;
    char bullet[DictionaryLookup::kMaxDefinitionLen + 1];
    snprintf(bullet, sizeof(bullet), "%s%s", kBullet, buf);
    emitLine(bullet);
    ++totalSenses;
    if (havePosOpen) ++sensesThisPos;
  };

  auto openPos = [&](const char* pos) {
    if (posBlocks >= kMaxPosBlocks) return false;
    // Blank line between POS sections (except before the first).
    if (posBlocks > 0 && w > 0 && w + 1 < sizeof(out)) {
      // emitLine already prefixes \n; use empty spacer via double newline by emitting pos after blank.
      if (out[w - 1] != '\n' && w + 1 < sizeof(out)) out[w++] = '\n';
      out[w] = '\0';
    }
    emitLine(pos);
    ++posBlocks;
    havePosOpen = true;
    sensesThisPos = 0;
    return true;
  };

  // Flat single-line multi-gloss (OEWN gap-fill): "a; b; c"
  if (strchr(def, '\n') == nullptr && strstr(def, "; ") != nullptr && strstr(def, " of ") == nullptr) {
    char tmp[DictionaryLookup::kMaxDefinitionLen + 1];
    strncpy(tmp, def, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char* save = nullptr;
    for (char* tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
      while (*tok == ' ') ++tok;
      if (*tok) emitSense(tok);
    }
    if (w > 0) {
      strncpy(def, out, cap - 1);
      def[cap - 1] = '\0';
    }
    return;
  }

  const char* p = def;
  while (*p && w + 16 < sizeof(out)) {
    const char* nl = strchr(p, '\n');
    char line[DictionaryLookup::kMaxDefinitionLen + 1];
    size_t len = nl ? static_cast<size_t>(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    p = nl ? nl + 1 : p + strlen(p);

    if (!line[0] || isOfCounter(line) || isSourceTag(line)) continue;

    if (line[0] == ':') {
      if (posBlocks == 0 && !havePosOpen) {
        // Sense before any POS — still show it
      } else if (posBlocks >= kMaxPosBlocks && sensesThisPos >= kMaxSensesPerPos) {
        continue;
      }
      emitSense(line + 1);
      continue;
    }

    if ((unsigned char)line[0] == 0xE2 && (unsigned char)line[1] == 0x80 && (unsigned char)line[2] == 0xA2) {
      const char* s = line + 3;
      if (*s == ' ') ++s;
      emitSense(s);
      continue;
    }
    if ((line[0] == '*' || line[0] == '-') && line[1] == ' ') {
      emitSense(line + 2);
      continue;
    }

    if (isPronLine(line)) {
      // Keep first pronunciation only; skip repeats (/wun/ x3).
      if (lastPron[0] && strcmp(lastPron, line) == 0) continue;
      if (!lastPron[0]) {
        strncpy(lastPron, line, sizeof(lastPron) - 1);
        lastPron[sizeof(lastPron) - 1] = '\0';
        // Only show IPA/enPR before any POS/senses (otherwise noise mid-card).
        if (posBlocks == 0 && totalSenses == 0) emitLine(line);
      }
      continue;
    }

    if (isPosLine(line)) {
      if (posBlocks >= kMaxPosBlocks) {
        // Ignore further POS blocks entirely (and their senses via sensesThisPos gate
        // once we leave havePosOpen maxed — mark closed).
        havePosOpen = false;
        sensesThisPos = kMaxSensesPerPos;
        continue;
      }
      if (!openPos(line)) {
        havePosOpen = false;
        sensesThisPos = kMaxSensesPerPos;
      }
      continue;
    }

    // Form-of context
    if (strstr(line, " of ") != nullptr && strlen(line) < 90) {
      emitLine(line);
      continue;
    }
    if (strlen(line) > 12) {
      emitSense(line);
    }
  }

  if (w > 0) {
    strncpy(def, out, cap - 1);
    def[cap - 1] = '\0';
  }
}

// Case-insensitive search for needle in haystack; returns pointer into haystack or null.
const char* strstri(const char* haystack, const char* needle) {
  if (!haystack || !needle || !*needle) return nullptr;
  for (const char* h = haystack; *h; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b &&
           (std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b)))) {
      ++a;
      ++b;
    }
    if (!*b) return h;
  }
  return nullptr;
}

// "in an obsequious manner" / "in a curious way" → base adjective "obsequious"/"curious".
bool extractMannerAdvBase(const char* def, char* outBase, const size_t outBaseLen) {
  if (!def || !outBase || outBaseLen < 3) return false;
  outBase[0] = '\0';
  // Keep this for short circular adverb glosses only (avoid long multi-sense entries).
  if (strlen(def) > 160) return false;

  static const char* kUnits[] = {" manner", " way", " fashion"};
  const char* unitAt = nullptr;
  size_t unitLen = 0;
  for (const char* unit : kUnits) {
    const char* p = strstri(def, unit);
    if (p && (!unitAt || p < unitAt)) {
      unitAt = p;
      unitLen = strlen(unit);
    }
  }
  if (!unitAt) return false;
  // Word immediately before " manner|way|fashion"
  const char* wordEnd = unitAt;
  while (wordEnd > def && std::isspace(static_cast<unsigned char>(wordEnd[-1]))) --wordEnd;
  const char* wordStart = wordEnd;
  while (wordStart > def && (std::isalpha(static_cast<unsigned char>(wordStart[-1])) || wordStart[-1] == '-' ||
                             wordStart[-1] == '\'')) {
    --wordStart;
  }
  if (wordStart >= wordEnd) return false;

  // Require "in a " / "in an " shortly before the adjective.
  const char* inAt = strstri(def, "in a");
  if (!inAt || inAt >= wordStart) return false;
  // "in a" or "in an" — skip article
  const char* afterIn = inAt + 4;  // past "in a"
  if (std::tolower(static_cast<unsigned char>(*afterIn)) == 'n') ++afterIn;
  while (*afterIn && std::isspace(static_cast<unsigned char>(*afterIn))) ++afterIn;
  // Allow only a short gap (article + spaces) so "in a kind of happy manner" is rejected.
  if (afterIn != wordStart) {
    // Tolerate a colon/bullet prefix on the same line: ": in an X manner"
    // already handled because inAt finds "in a". Reject if extra words between article and adj.
    return false;
  }

  char tmp[DictionaryLookup::kMaxKeyLen];
  const size_t wlen = static_cast<size_t>(wordEnd - wordStart);
  if (wlen < 2 || wlen >= sizeof(tmp)) return false;
  memcpy(tmp, wordStart, wlen);
  tmp[wlen] = '\0';
  if (!DictionaryLookup::normalizeWord(tmp, sizeof(tmp))) return false;
  if (strlen(tmp) < 2) return false;
  strncpy(outBase, tmp, outBaseLen - 1);
  outBase[outBaseLen - 1] = '\0';
  (void)unitLen;
  return true;
}

// If def is mainly "… of baseWord" or "in an X manner", extract base into outBase.
bool extractFormOfBase(const char* def, char* outBase, const size_t outBaseLen) {
  if (!def || !outBase || outBaseLen < 3) return false;
  outBase[0] = '\0';

  // Circular adverb glosses (WordNet/Wiktionary): "in an obsequious manner."
  if (extractMannerAdvBase(def, outBase, outBaseLen)) return true;

  // Match common Wiktionary inflection glosses: take text after the last " of ".
  // Examples: "plural of missile", "present participle and gerund of gag"
  const char* ofPos = nullptr;
  for (const char* p = def; *p; ++p) {
    if ((p == def || !std::isalpha(static_cast<unsigned char>(p[-1]))) && p[0] == 'o' && p[1] == 'f' && p[2] == ' ' &&
        (p == def || p[-1] == ' ')) {
      ofPos = p + 3;
    }
  }
  if (!ofPos || !*ofPos) return false;

  // Only treat as form-of when the gloss is short / mostly inflectional.
  // Avoid "a type of fish that..." style definitions.
  const size_t defLen = strlen(def);
  if (defLen > 120) return false;
  static const char* kMarkers[] = {
      "plural of ",
      "participle of ",
      "gerund of ",
      "past of ",
      "form of ",
      "tense of ",
      "indicative of ",
      "comparative of ",
      "superlative of ",
      "singular of ",
      "adverb of ",
      "adverbial form of ",
      "adverb form of ",
  };
  bool looksFormOf = false;
  for (const char* m : kMarkers) {
    if (strstr(def, m) != nullptr) {
      looksFormOf = true;
      break;
    }
  }
  if (!looksFormOf) return false;

  char tmp[DictionaryLookup::kMaxKeyLen];
  strncpy(tmp, ofPos, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  // Cut at punctuation / semicolon / period / paren
  for (char* p = tmp; *p; ++p) {
    if (*p == ';' || *p == '.' || *p == ',' || *p == '(' || *p == '[' || *p == '|' || *p == '/') {
      *p = '\0';
      break;
    }
  }
  if (!DictionaryLookup::normalizeWord(tmp, sizeof(tmp))) return false;
  if (strlen(tmp) < 2) return false;
  strncpy(outBase, tmp, outBaseLen - 1);
  outBase[outBaseLen - 1] = '\0';
  return true;
}

// Look up key in an already-open file (exact + stem + Spanish clitics).
// If found via a Spanish clitic strip, writes the clitic into outClitic (optional).
// If found under a different stem key, writes that into outMatchedKey (optional).
bool searchOpenFile(HalFile& file, const DictHeader& hdr, const char* originalKey, IndexEntry& entry,
                    char* outClitic = nullptr, size_t outCliticLen = 0, char* outMatchedKey = nullptr,
                    size_t outMatchedKeyLen = 0) {
  if (outClitic && outCliticLen) outClitic[0] = '\0';
  if (outMatchedKey && outMatchedKeyLen) outMatchedKey[0] = '\0';

  auto rememberKey = [&](const char* k) {
    if (outMatchedKey && outMatchedKeyLen > 1 && k) {
      strncpy(outMatchedKey, k, outMatchedKeyLen - 1);
      outMatchedKey[outMatchedKeyLen - 1] = '\0';
    }
  };

  char key[DictionaryLookup::kMaxKeyLen];
  strncpy(key, originalKey, sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  if (binarySearch(file, hdr, key, entry)) {
    rememberKey(key);
    return true;
  }

  // Dedicated Spanish clitic pass first: ayudame → ayuda (before English stems).
  {
    char cliticStem[DictionaryLookup::kMaxKeyLen];
    strncpy(cliticStem, key, sizeof(cliticStem) - 1);
    cliticStem[sizeof(cliticStem) - 1] = '\0';
    char clitic[12] = {};
    char allClitics[24] = {};
    for (int attempt = 0; attempt < 4; ++attempt) {
      char one[12] = {};
      if (!trySpanishCliticStrip(cliticStem, one, sizeof(one))) break;
      if (allClitics[0] && strlen(allClitics) + 1 < sizeof(allClitics)) {
        strncat(allClitics, "+", sizeof(allClitics) - strlen(allClitics) - 1);
      }
      strncat(allClitics, one, sizeof(allClitics) - strlen(allClitics) - 1);
      if (binarySearch(file, hdr, cliticStem, entry)) {
        if (outClitic && outCliticLen > 1) {
          strncpy(outClitic, allClitics[0] ? allClitics : one, outCliticLen - 1);
          outClitic[outCliticLen - 1] = '\0';
        }
        rememberKey(cliticStem);
        return true;
      }
      // Spanish infinitive guess: ayuda → ayudar (common for ES→EN packs).
      if (strlen(cliticStem) + 2 < DictionaryLookup::kMaxKeyLen) {
        char infinitive[DictionaryLookup::kMaxKeyLen];
        snprintf(infinitive, sizeof(infinitive), "%sar", cliticStem);
        if (binarySearch(file, hdr, infinitive, entry)) {
          if (outClitic && outCliticLen > 1) {
            strncpy(outClitic, allClitics[0] ? allClitics : one, outCliticLen - 1);
            outClitic[outCliticLen - 1] = '\0';
          }
          rememberKey(infinitive);
          return true;
        }
        snprintf(infinitive, sizeof(infinitive), "%ser", cliticStem);
        if (binarySearch(file, hdr, infinitive, entry)) {
          if (outClitic && outCliticLen > 1) {
            strncpy(outClitic, allClitics[0] ? allClitics : one, outCliticLen - 1);
            outClitic[outCliticLen - 1] = '\0';
          }
          rememberKey(infinitive);
          return true;
        }
        snprintf(infinitive, sizeof(infinitive), "%sir", cliticStem);
        if (binarySearch(file, hdr, infinitive, entry)) {
          if (outClitic && outCliticLen > 1) {
            strncpy(outClitic, allClitics[0] ? allClitics : one, outCliticLen - 1);
            outClitic[outCliticLen - 1] = '\0';
          }
          rememberKey(infinitive);
          return true;
        }
      }
      strncpy(clitic, one, sizeof(clitic) - 1);
    }
  }

  char stem[DictionaryLookup::kMaxKeyLen];
  strncpy(stem, key, sizeof(stem) - 1);
  stem[sizeof(stem) - 1] = '\0';
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (!tryStemVariants(stem)) break;
    if (binarySearch(file, hdr, stem, entry)) {
      rememberKey(stem);
      return true;
    }
    // simply → simp then simple; gently → gent then gentle.
    if (strlen(stem) + 1 < DictionaryLookup::kMaxKeyLen) {
      char withE[DictionaryLookup::kMaxKeyLen];
      snprintf(withE, sizeof(withE), "%se", stem);
      if (binarySearch(file, hdr, withE, entry)) {
        rememberKey(withE);
        return true;
      }
    }
  }

  char folded[DictionaryLookup::kMaxKeyLen];
  strncpy(folded, key, sizeof(folded) - 1);
  folded[sizeof(folded) - 1] = '\0';
  if (foldAccentsInPlace(folded) && strcmp(folded, key) != 0) {
    if (binarySearch(file, hdr, folded, entry)) {
      rememberKey(folded);
      return true;
    }
    // Clitics on accent-folded form (ayudame with accents already gone).
    char cliticStem[DictionaryLookup::kMaxKeyLen];
    strncpy(cliticStem, folded, sizeof(cliticStem) - 1);
    cliticStem[sizeof(cliticStem) - 1] = '\0';
    char allClitics[24] = {};
    for (int attempt = 0; attempt < 4; ++attempt) {
      char one[12] = {};
      if (!trySpanishCliticStrip(cliticStem, one, sizeof(one))) break;
      if (allClitics[0] && strlen(allClitics) + 1 < sizeof(allClitics)) {
        strncat(allClitics, "+", sizeof(allClitics) - strlen(allClitics) - 1);
      }
      strncat(allClitics, one, sizeof(allClitics) - strlen(allClitics) - 1);
      auto setCliticOut = [&]() {
        if (outClitic && outCliticLen > 1) {
          strncpy(outClitic, allClitics[0] ? allClitics : one, outCliticLen - 1);
          outClitic[outCliticLen - 1] = '\0';
        }
      };
      if (binarySearch(file, hdr, cliticStem, entry)) {
        setCliticOut();
        rememberKey(cliticStem);
        return true;
      }
      // Same infinitive guess as the non-folded path (ayuda → ayudar).
      if (strlen(cliticStem) + 2 < DictionaryLookup::kMaxKeyLen) {
        char infinitive[DictionaryLookup::kMaxKeyLen];
        for (const char* suf : {"ar", "er", "ir"}) {
          snprintf(infinitive, sizeof(infinitive), "%s%s", cliticStem, suf);
          if (binarySearch(file, hdr, infinitive, entry)) {
            setCliticOut();
            rememberKey(infinitive);
            return true;
          }
        }
      }
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (!tryStemVariants(folded)) break;
      if (binarySearch(file, hdr, folded, entry)) {
        rememberKey(folded);
        return true;
      }
    }
  }
  return false;
}

bool appendClamped(char* dest, const size_t destLen, const char* src) {
  if (!dest || destLen == 0 || !src) return false;
  const size_t used = strlen(dest);
  if (used + 1 >= destLen) return false;
  const size_t room = destLen - used - 1;
  const size_t n = strnlen(src, room);
  memcpy(dest + used, src, n);
  dest[used + n] = '\0';
  return n > 0;
}

// Expand "plural of X" etc. by also loading X's definition from the same pack.
void expandFormOfIfNeeded(HalFile& file, const DictHeader& hdr, char* def, const size_t defLen) {
  if (!def || defLen < 32) return;

  char base[DictionaryLookup::kMaxKeyLen];
  if (!extractFormOfBase(def, base, sizeof(base))) return;

  IndexEntry entry{};
  if (!searchOpenFile(file, hdr, base, entry)) return;

  char baseDef[DictionaryLookup::kMaxDefinitionLen + 1];
  if (!loadDefinition(file, hdr, entry, baseDef, sizeof(baseDef))) return;

  // Avoid infinite-ish chains: if base is also form-of, still show it once.
  // Format: "<form gloss>. <base>: <baseDef>"
  char combined[DictionaryLookup::kMaxDefinitionLen + 1];
  combined[0] = '\0';
  appendClamped(combined, sizeof(combined), def);
  // Trim trailing junk spaces
  size_t n = strlen(combined);
  while (n > 0 && (combined[n - 1] == ' ' || combined[n - 1] == ';')) {
    combined[--n] = '\0';
  }
  if (n > 0 && combined[n - 1] != '.') {
    appendClamped(combined, sizeof(combined), ".");
  }
  appendClamped(combined, sizeof(combined), " ");
  appendClamped(combined, sizeof(combined), base);
  appendClamped(combined, sizeof(combined), ": ");
  appendClamped(combined, sizeof(combined), baseDef);

  strncpy(def, combined, defLen - 1);
  def[defLen - 1] = '\0';
}

// Session: keep packs open while dictionary UI is active (beginSession/endSession).
struct SessionPack {
  HalFile file;
  DictHeader hdr{};
  bool open = false;
  const char* path = nullptr;
  const char* label = nullptr;
};

constexpr size_t kMaxSessionPacks = 4;
SessionPack s_sessionPacks[kMaxSessionPacks];
uint8_t s_sessionPackCount = 0;
bool s_sessionActive = false;

SessionPack* findSessionPack(const char* path) {
  if (!s_sessionActive || !path) return nullptr;
  for (uint8_t i = 0; i < s_sessionPackCount; ++i) {
    if (s_sessionPacks[i].open && s_sessionPacks[i].path && strcmp(s_sessionPacks[i].path, path) == 0) {
      return &s_sessionPacks[i];
    }
  }
  return nullptr;
}

bool lookupInOpenPack(HalFile& file, const DictHeader& hdr, const char* originalKey, char* outDef,
                      const size_t outDefLen) {
  IndexEntry entry{};
  char clitic[24] = {};
  char matchedKey[DictionaryLookup::kMaxKeyLen] = {};
  if (!searchOpenFile(file, hdr, originalKey, entry, clitic, sizeof(clitic), matchedKey, sizeof(matchedKey))) {
    return false;
  }

  if (!loadDefinition(file, hdr, entry, outDef, outDefLen)) {
    return false;
  }

  expandFormOfIfNeeded(file, hdr, outDef, outDefLen);
  formatDefBodyForUi(outDef, outDefLen);

  // Spanish clitic decoration sits above the formatted senses.
  if (clitic[0] != '\0' && outDef[0] != '\0') {
    char decorated[DictionaryLookup::kMaxDefinitionLen + 1];
    const char* head = matchedKey[0] ? matchedKey : originalKey;
    const char* gloss = esCliticGloss(clitic);
    char natural[64] = {};
    const bool hasNatural = composeCliticEnglishPhrase(clitic, outDef, natural, sizeof(natural));
    if (hasNatural && gloss && gloss[0]) {
      snprintf(decorated, sizeof(decorated), "%s\n%s (%s)\n%s", natural, head, gloss, outDef);
    } else if (hasNatural) {
      snprintf(decorated, sizeof(decorated), "%s\n%s\n%s", natural, head, outDef);
    } else if (gloss && gloss[0]) {
      snprintf(decorated, sizeof(decorated), "%s (%s)\n%s", head, gloss, outDef);
    } else {
      snprintf(decorated, sizeof(decorated), "%s\n%s", head, outDef);
    }
    strncpy(outDef, decorated, outDefLen - 1);
    outDef[outDefLen - 1] = '\0';
  }
  return outDef[0] != '\0';
}

bool lookupInFile(const char* path, const char* originalKey, char* outDef, const size_t outDefLen) {
  if (SessionPack* sp = findSessionPack(path)) {
    return lookupInOpenPack(sp->file, sp->hdr, originalKey, outDef, outDefLen);
  }

  if (!Storage.exists(path)) return false;

  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) {
    LOG_ERR("DICT", "Failed to open %s", path);
    return false;
  }

  DictHeader hdr{};
  if (!readHeader(file, hdr)) {
    LOG_ERR("DICT", "Invalid dictionary header: %s", path);
    file.close();
    return false;
  }

  const bool ok = lookupInOpenPack(file, hdr, originalKey, outDef, outDefLen);
  file.close();
  return ok;
}

const char* pathForLang(const DictionaryLookup::Lang lang) {
  switch (lang) {
    case DictionaryLookup::Lang::En:
      return DictionaryLookup::kPathEn;
    case DictionaryLookup::Lang::Es:
      return DictionaryLookup::kPathEs;
    case DictionaryLookup::Lang::EnEs:
      return DictionaryLookup::kPathEnEs;
    case DictionaryLookup::Lang::EsEn:
      return DictionaryLookup::kPathEsEn;
    case DictionaryLookup::Lang::Auto:
    default:
      return DictionaryLookup::kPathEn;
  }
}

}  // namespace

bool DictionaryLookup::available(const char* path) {
  if (path == nullptr || path[0] == '\0') return false;
  if (findSessionPack(path)) return true;
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) return false;
  DictHeader hdr{};
  const bool ok = readHeader(file, hdr);
  file.close();
  return ok;
}

bool DictionaryLookup::anyAvailable() {
  if (s_sessionActive) return s_sessionPackCount > 0;
  for (const Pack& p : kPacks) {
    if (available(p.path)) return true;
  }
  return false;
}

void DictionaryLookup::beginSession() {
  endSession();
  for (const Pack& p : kPacks) {
    if (s_sessionPackCount >= kMaxSessionPacks) break;
    if (!Storage.exists(p.path)) continue;
    SessionPack& sp = s_sessionPacks[s_sessionPackCount];
    if (!Storage.openFileForRead("DICT", p.path, sp.file)) {
      LOG_ERR("DICT", "session open failed: %s", p.path);
      continue;
    }
    if (!readHeader(sp.file, sp.hdr)) {
      LOG_ERR("DICT", "session bad header: %s", p.path);
      sp.file.close();
      continue;
    }
    sp.open = true;
    sp.path = p.path;
    sp.label = p.label;
    s_sessionPackCount++;
  }
  s_sessionActive = true;
  LOG_INF("DICT", "session begin packs=%u free=%u", static_cast<unsigned>(s_sessionPackCount),
          static_cast<unsigned>(ESP.getFreeHeap()));
}

void DictionaryLookup::endSession() {
  if (!s_sessionActive && s_sessionPackCount == 0) return;
  for (uint8_t i = 0; i < s_sessionPackCount; ++i) {
    if (s_sessionPacks[i].open) {
      s_sessionPacks[i].file.close();
      s_sessionPacks[i].open = false;
      s_sessionPacks[i].path = nullptr;
      s_sessionPacks[i].label = nullptr;
    }
  }
  s_sessionPackCount = 0;
  s_sessionActive = false;
  LOG_INF("DICT", "session end free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
}

bool DictionaryLookup::sessionActive() { return s_sessionActive; }

bool DictionaryLookup::normalizeWord(char* word, const size_t cap) {
  if (word == nullptr || cap == 0) return false;

  // Build a new key in-place: lowercase Latin letters + Spanish letters as UTF-8,
  // ASCII hyphens (compounds), and single spaces (multi-word phrases).
  // Strips quotes, other punctuation, digits, symbols (including curly quotes / dashes).
  unsigned char out[kMaxKeyLen];
  size_t w = 0;

  const unsigned char* r = reinterpret_cast<const unsigned char*>(word);
  while (*r && w + 4 < cap && w + 4 < kMaxKeyLen) {
    // ASCII
    if (*r < 0x80) {
      unsigned char c = *r++;
      if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
      // Letters, hyphen for compounds (fetid-smelling), space for multi-word.
      // Apostrophes stay stripped so "it'll" → "itll" (contraction expansion).
      if (c >= 'a' && c <= 'z') {
        out[w++] = c;
      } else if (c == '-' && w > 0 && out[w - 1] != '-' && out[w - 1] != ' ') {
        out[w++] = c;
      } else if ((c == ' ' || c == '\t') && w > 0 && out[w - 1] != ' ' && out[w - 1] != '-') {
        out[w++] = ' ';
      }
      continue;
    }
    // 2-byte UTF-8 (covers Spanish accents in Latin-1 Supplement)
    if ((*r & 0xE0) == 0xC0 && r[1] != 0) {
      unsigned char b0 = r[0];
      unsigned char b1 = r[1];
      if (b0 == 0xC3) {
        if (b1 >= 0x80 && b1 <= 0x9E) {
          if (b1 == 0x81)
            b1 = 0xA1;
          else if (b1 == 0x89)
            b1 = 0xA9;
          else if (b1 == 0x8D)
            b1 = 0xAD;
          else if (b1 == 0x93)
            b1 = 0xB3;
          else if (b1 == 0x9A)
            b1 = 0xBA;
          else if (b1 == 0x9C)
            b1 = 0xBC;
          else if (b1 == 0x91)
            b1 = 0xB1;
        }
        // Keep Spanish letters áéíóúüñ; drop other C3 symbols.
        if (b1 == 0xA1 || b1 == 0xA9 || b1 == 0xAD || b1 == 0xB3 || b1 == 0xBA || b1 == 0xBC || b1 == 0xB1) {
          out[w++] = b0;
          out[w++] = b1;
        }
      }
      // Soft hyphen C2 AD and similar: skip
      r += 2;
      continue;
    }
    // 3-byte: curly quotes “ ” ‘ ’, dashes, etc. — skip entirely
    if ((*r & 0xF0) == 0xE0 && r[1] && r[2]) {
      r += 3;
      continue;
    }
    if ((*r & 0xF8) == 0xF0 && r[1] && r[2] && r[3]) {
      r += 4;
      continue;
    }
    ++r;
  }
  // Trim trailing hyphen/space so "well- " / "a " do not leave junk keys.
  while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '-')) {
    --w;
  }
  out[w] = 0;
  memcpy(word, out, w + 1);
  return w > 0;
}

// Ultra-common English closed-class words. Some packs omit them (single-letter
// filter, trim quirks, or bilingual-only crumbs like EN->ES "y, e" for "and").
// Keep defs short for the e-ink card; formatDefBodyForUi still applies.
bool lookupBuiltinEnglish(const char* key, char* outDef, const size_t outDefLen) {
  if (!key || !outDef || outDefLen < 8) return false;
  struct Builtin {
    const char* k;
    const char* d;
  };
  // Sorted for binary search by first letter length; linear is fine (tiny table).
  static const Builtin kBuiltins[] = {
      // "a" is missing from many compact EN packs; keep this usable when Auto also
      // pulls a long Spanish ES->EN dump for Spanish "a" (letter / preposition / ending).
      {"a",
       "article\n: used before a singular noun phrase when the thing is not specific "
       "(a book, a day)\n: one; any; each (a mile a minute)"},
      {"an", "article\n: form of a used before a vowel sound (an apple, an hour)"},
      {"and", "conjunction\n: connects words or clauses; also; plus\n: then; next in a sequence"},
      {"because", "conjunction\n: for the reason that; since"},
      {"against", "preposition\n: in opposition to\n: next to; touching"},
      {"i", "pronoun\n: the speaker or writer; first person singular"},
  };
  for (const Builtin& b : kBuiltins) {
    if (strcmp(key, b.k) == 0) {
      char tmp[DictionaryLookup::kMaxDefinitionLen + 1];
      strncpy(tmp, b.d, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      formatDefBodyForUi(tmp, sizeof(tmp));
      strncpy(outDef, tmp, outDefLen - 1);
      outDef[outDefLen - 1] = '\0';
      return outDef[0] != '\0';
    }
  }
  return false;
}

bool keyInList(const char* key, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (strcmp(key, list[i]) == 0) return true;
  }
  return false;
}

// Apostrophes are stripped by normalizeWord ("it'll" → "itll"). Many packs store
// dont/wont/cant but miss itll/theyre/etc. Expand to base + meaning, then look up base.
// Returns true if outDef was filled.
bool lookupEnglishContraction(const char* key, char* outDef, const size_t outDefLen) {
  if (!key || !outDef || outDefLen < 16) return false;
  const size_t n = strlen(key);
  if (n < 3 || n >= DictionaryLookup::kMaxKeyLen) return false;

  char base[DictionaryLookup::kMaxKeyLen];
  const char* expansion = nullptr;

  auto setBase = [&](size_t baseLen, const char* exp) {
    if (baseLen == 0 || baseLen >= sizeof(base)) return false;
    memcpy(base, key, baseLen);
    base[baseLen] = '\0';
    expansion = exp;
    return true;
  };

  // Longest suffixes first. key is already lowercase ASCII letters only.
  if (n >= 4 && key[n - 2] == 'l' && key[n - 1] == 'l') {
    // it'll, we'll, they'll, who'll, that'll, what'll, ...
    if (!setBase(n - 2, "will")) return false;
  } else if (n >= 4 && key[n - 2] == 'r' && key[n - 1] == 'e') {
    // you're, they're, we're, who're
    static const char* kRe[] = {"you", "they", "we", "who", "what", "that", "there"};
    if (!setBase(n - 2, "are")) return false;
    if (!keyInList(base, kRe, sizeof(kRe) / sizeof(kRe[0]))) return false;
  } else if (n >= 4 && key[n - 2] == 'v' && key[n - 1] == 'e') {
    // I've, you've, they've, would've, should've, could've
    if (!setBase(n - 2, "have")) return false;
  } else if (n >= 3 && key[n - 1] == 'm') {
    // I'm
    if (!setBase(n - 1, "am")) return false;
    if (strcmp(base, "i") != 0) return false;
  } else if (n >= 4 && key[n - 2] == 'n' && key[n - 1] == 't') {
    // isn't, don't, won't, can't, wouldn't, ... (pack often has these, but not all)
    static const char* kNt[] = {"do",  "does", "did",  "is",   "are",  "was",  "were", "has",  "have",
                                "had", "would", "should", "could", "might", "must", "can", "will", "did",
                                "need", "dare", "used", "ought", "ai", "wo", "sha", "ca"};
    if (!setBase(n - 2, "not")) return false;
    if (!keyInList(base, kNt, sizeof(kNt) / sizeof(kNt[0]))) return false;
    // Spelling normalizations used in speech: won't, can't, shan't, ain't
    if (strcmp(base, "wo") == 0) {
      strncpy(base, "will", sizeof(base) - 1);
      base[sizeof(base) - 1] = '\0';
    } else if (strcmp(base, "ca") == 0) {
      strncpy(base, "can", sizeof(base) - 1);
      base[sizeof(base) - 1] = '\0';
    } else if (strcmp(base, "sha") == 0) {
      strncpy(base, "shall", sizeof(base) - 1);
      base[sizeof(base) - 1] = '\0';
    } else if (strcmp(base, "ai") == 0) {
      // ain't → am/is/are not — no single base
      char tmp[DictionaryLookup::kMaxDefinitionLen + 1];
      snprintf(tmp, sizeof(tmp),
               "contraction\n: short for am not, is not, or are not (informal)");
      formatDefBodyForUi(tmp, sizeof(tmp));
      strncpy(outDef, tmp, outDefLen - 1);
      outDef[outDefLen - 1] = '\0';
      return true;
    }
  } else if (n >= 3 && key[n - 1] == 's') {
    // it's, that's, what's, who's, here's, there's, let's
    static const char* kS[] = {"it", "that", "what", "who", "where", "here", "there", "how", "let", "he", "she"};
    if (!setBase(n - 1, "is")) return false;  // also "has" in speech; show is as primary
    if (!keyInList(base, kS, sizeof(kS) / sizeof(kS[0]))) return false;
    if (strcmp(base, "let") == 0) {
      expansion = "us";  // let's
    }
  } else if (n >= 3 && key[n - 1] == 'd') {
    // I'd, you'd, he'd, they'd, it'd
    static const char* kD[] = {"i", "you", "he", "she", "we", "they", "it", "who", "that", "there"};
    if (!setBase(n - 1, "would")) return false;  // also had
    if (!keyInList(base, kD, sizeof(kD) / sizeof(kD[0]))) return false;
    expansion = "would / had";
  } else {
    return false;
  }

  // Prefer looking up the base lemma in the English pack / builtins.
  char baseDef[DictionaryLookup::kMaxDefinitionLen + 1];
  baseDef[0] = '\0';
  if (!lookupInFile(DictionaryLookup::kPathEn, base, baseDef, sizeof(baseDef))) {
    if (!lookupBuiltinEnglish(base, baseDef, sizeof(baseDef))) {
      baseDef[0] = '\0';
    }
  }

  // baseDef is already UI-formatted when loaded from pack/builtin. Do not run
  // formatDefBodyForUi on the combined string or multi-POS bases get re-capped badly.
  char combined[DictionaryLookup::kMaxDefinitionLen + 1];
  if (baseDef[0]) {
    snprintf(combined, sizeof(combined), "contraction\n• short for \"%s %s\"\n\n%s", base, expansion, baseDef);
  } else {
    snprintf(combined, sizeof(combined), "contraction\n• short for \"%s %s\"", base, expansion);
  }
  strncpy(outDef, combined, outDefLen - 1);
  outDef[outDefLen - 1] = '\0';
  return outDef[0] != '\0';
}

// English EN pack: exact key → builtin closed-class → contraction expansion.
bool tryEnglishKey(const char* key, char* outDef, const size_t outDefLen) {
  if (!key || !key[0] || !outDef || outDefLen == 0) return false;
  if (lookupInFile(DictionaryLookup::kPathEn, key, outDef, outDefLen)) return true;
  if (lookupBuiltinEnglish(key, outDef, outDefLen)) return true;
  if (lookupEnglishContraction(key, outDef, outDefLen)) return true;
  return false;
}

// Hyphen / multi-word compounds: try compact form, space↔hyphen swap, then each segment.
// "fetid-smelling" → fetidsmelling → "fetid smelling" → fetid → smelling.
template <typename TryFn>
bool lookupCompoundVariants(const char* key, char* outDef, const size_t outDefLen, TryFn&& tryKey) {
  if (!key || (!strchr(key, '-') && !strchr(key, ' '))) return false;

  char compact[DictionaryLookup::kMaxKeyLen];
  size_t cw = 0;
  for (const char* p = key; *p && cw + 1 < sizeof(compact); ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c >= 'a' && c <= 'z') || (c & 0x80)) {
      compact[cw++] = static_cast<char>(c);
    }
  }
  compact[cw] = '\0';
  if (cw > 0 && strcmp(compact, key) != 0 && tryKey(compact, outDef, outDefLen)) {
    return true;
  }

  char swapped[DictionaryLookup::kMaxKeyLen];
  size_t sw = 0;
  for (const char* p = key; *p && sw + 1 < sizeof(swapped); ++p) {
    char c = *p;
    if (c == '-')
      c = ' ';
    else if (c == ' ')
      c = '-';
    swapped[sw++] = c;
  }
  swapped[sw] = '\0';
  if (sw > 0 && strcmp(swapped, key) != 0 && tryKey(swapped, outDef, outDefLen)) {
    return true;
  }

  char part[DictionaryLookup::kMaxKeyLen];
  size_t pw = 0;
  auto flushPart = [&]() -> bool {
    while (pw > 0 && (part[pw - 1] == ' ' || part[pw - 1] == '-')) --pw;
    part[pw] = '\0';
    const bool ok = pw > 0 && tryKey(part, outDef, outDefLen);
    pw = 0;
    return ok;
  };
  for (const char* p = key; *p; ++p) {
    if (*p == '-' || *p == ' ') {
      if (flushPart()) return true;
      continue;
    }
    if (pw + 1 < sizeof(part)) part[pw++] = *p;
  }
  return flushPart();
}

bool lookupEnglishWithFallbacks(const char* key, char* outDef, const size_t outDefLen) {
  if (tryEnglishKey(key, outDef, outDefLen)) return true;
  return lookupCompoundVariants(key, outDef, outDefLen,
                                [](const char* k, char* d, size_t n) { return tryEnglishKey(k, d, n); });
}

// True for bare bilingual glosses like "y, e" or "el, la" with no POS/senses layout.
bool isBareBilingualGloss(const char* def) {
  if (!def || !def[0] || strchr(def, '\n') != nullptr) return false;
  if (strlen(def) > 48) return false;
  // Mostly short tokens separated by commas — not a full English sentence.
  int letters = 0, commas = 0, spaces = 0;
  for (const char* p = def; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c & 0x80)) letters++;
    else if (c == ',') commas++;
    else if (c == ' ') spaces++;
    else if (c != '-' && c != ';' && c != '.') return false;
  }
  return letters >= 1 && letters <= 24 && commas >= 1;
}

void polishBilingualSection(char* section, const size_t sectionLen, const char* packLabel) {
  if (!section || !section[0] || !packLabel) return;
  if (!isBareBilingualGloss(section)) return;
  // "y, e" → clearer bilingual card body
  char polished[DictionaryLookup::kMaxDefinitionLen + 1];
  if (strcmp(packLabel, "EN->ES") == 0) {
    snprintf(polished, sizeof(polished), "Spanish\n: %s", section);
  } else if (strcmp(packLabel, "ES->EN") == 0) {
    snprintf(polished, sizeof(polished), "English\n: %s", section);
  } else {
    snprintf(polished, sizeof(polished), ": %s", section);
  }
  formatDefBodyForUi(polished, sizeof(polished));
  strncpy(section, polished, sectionLen - 1);
  section[sectionLen - 1] = '\0';
}

bool DictionaryLookup::lookup(const char* word, char* outDef, const size_t outDefLen, const char* path) {
  if (word == nullptr || outDef == nullptr || outDefLen == 0) return false;
  outDef[0] = '\0';

  char key[kMaxKeyLen];
  strncpy(key, word, sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';
  if (!normalizeWord(key, sizeof(key))) {
    LOG_DBG("DICT", "Empty key after normalize");
    return false;
  }

  if (lookupInFile(path, key, outDef, outDefLen)) return true;
  // English pack path: closed-class builtins + contraction expansion (it'll → it will).
  if (path && (strcmp(path, kPathEn) == 0 || strcmp(path, kDefaultPath) == 0)) {
    if (lookupBuiltinEnglish(key, outDef, outDefLen)) return true;
    return lookupEnglishContraction(key, outDef, outDefLen);
  }
  return false;
}

bool DictionaryLookup::lookupAuto(const char* word, char* outDef, const size_t outDefLen, char* outLangLabel,
                                  const size_t outLangLabelLen, const Lang prefer) {
  if (outDef) outDef[0] = '\0';
  if (outLangLabel && outLangLabelLen) outLangLabel[0] = '\0';

  char key[kMaxKeyLen];
  if (word == nullptr || outDef == nullptr || outDefLen == 0) return false;
  strncpy(key, word, sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';
  if (!normalizeWord(key, sizeof(key))) return false;

  auto tryPath = [&](const char* path, const char* label, char* defBuf, size_t defBufLen) -> bool {
    if (label && strcmp(label, "EN") == 0) {
      return lookupEnglishWithFallbacks(key, defBuf, defBufLen) && defBuf[0] != '\0';
    }
    if (lookupInFile(path, key, defBuf, defBufLen) && defBuf[0] != '\0' && label != nullptr) {
      return true;
    }
    // Same compound/segment retries as English for bilingual packs.
    return label != nullptr &&
           lookupCompoundVariants(key, defBuf, defBufLen, [path](const char* k, char* d, size_t n) {
             return lookupInFile(path, k, d, n) && d[0] != '\0';
           });
  };

  if (prefer != Lang::Auto) {
    const char* path = pathForLang(prefer);
    const char* label = "DICT";
    for (const Pack& p : kPacks) {
      if (strcmp(p.path, path) == 0) {
        label = p.label;
        break;
      }
    }
    if (prefer == Lang::En || (path && strcmp(path, kPathEn) == 0)) {
      if (!lookupEnglishWithFallbacks(key, outDef, outDefLen)) return false;
    } else if (!tryPath(path, label, outDef, outDefLen)) {
      return false;
    }
    if (outLangLabel && outLangLabelLen > 0) {
      strncpy(outLangLabel, label, outLangLabelLen - 1);
      outLangLabel[outLangLabelLen - 1] = '\0';
    }
    return true;
  }

  // Auto: collect hits from every installed pack so Spanish + English both show
  // when both match (e.g. "por"). Layout for the popup:
  //   @EN          ← pack header (drawn bold; '@' stripped in UI)
  //   noun
  //   • sense…
  //
  //   @ES->EN
  //   • translation…
  char section[DictionaryLookup::kMaxDefinitionLen + 1];
  outDef[0] = '\0';
  char labels[DictionaryLookup::kMaxLangLabelLen];
  labels[0] = '\0';
  int hits = 0;
  bool haveEnglish = false;

  auto appendHit = [&](const char* packLabel, const char* body) {
    if (!packLabel || !body || !body[0]) return;
    if (hits > 0 && strstr(outDef, body) != nullptr) return;

    char piece[DictionaryLookup::kMaxDefinitionLen + 1];
    if (hits == 0) {
      snprintf(outDef, outDefLen, "%s", body);
    } else if (hits == 1) {
      char firstLabel[16] = "EN";
      if (labels[0]) {
        strncpy(firstLabel, labels, sizeof(firstLabel) - 1);
        firstLabel[sizeof(firstLabel) - 1] = '\0';
        char* plus = strchr(firstLabel, '+');
        if (plus) *plus = '\0';
      }
      char promoted[DictionaryLookup::kMaxDefinitionLen + 1];
      snprintf(promoted, sizeof(promoted), "@%s\n%s\n\n@%s\n%s", firstLabel, outDef, packLabel, body);
      snprintf(outDef, outDefLen, "%s", promoted);
    } else {
      snprintf(piece, sizeof(piece), "\n\n@%s\n%s", packLabel, body);
      appendClamped(outDef, outDefLen, piece);
    }

    if (hits == 0) {
      strncpy(labels, packLabel, sizeof(labels) - 1);
      labels[sizeof(labels) - 1] = '\0';
    } else {
      char labPiece[16];
      snprintf(labPiece, sizeof(labPiece), "+%s", packLabel);
      appendClamped(labels, sizeof(labels), labPiece);
    }
    ++hits;
  };

  for (const Pack& p : kPacks) {
    section[0] = '\0';
    // EN path includes pack + builtins + contraction expansion (it'll, they're, …).
    const bool ok = tryPath(p.path, p.label, section, sizeof(section));
    if (!ok) continue;

    if (strcmp(p.label, "EN") == 0) {
      haveEnglish = true;
    } else {
      polishBilingualSection(section, sizeof(section), p.label);
    }

    // Single-letter keys (a, y, o, …) often mean different languages. After we
    // already have English, ES / ES->EN dumps for Spanish "a" drown the card
    // (letter name, personal-a, verb endings…). Keep EN + EN->ES only.
    // Spanish-only lookups still get ES->EN when EN did not hit.
    if (haveEnglish && strlen(key) <= 1 &&
        (strcmp(p.label, "ES") == 0 || strcmp(p.label, "ES->EN") == 0)) {
      continue;
    }

    appendHit(p.label, section);
    if (hits >= 3) break;
  }

  // No pack at all (or EN file absent): still serve built-in closed-class English.
  if (hits == 0 && lookupBuiltinEnglish(key, outDef, outDefLen)) {
    strncpy(labels, "EN", sizeof(labels) - 1);
    labels[sizeof(labels) - 1] = '\0';
    hits = 1;
    haveEnglish = true;
  }
  (void)haveEnglish;

  if (hits == 0) return false;

  if (outLangLabel && outLangLabelLen > 0) {
    strncpy(outLangLabel, labels, outLangLabelLen - 1);
    outLangLabel[outLangLabelLen - 1] = '\0';
  }
  return true;
}
