#include "Dictionary.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include "DictZip.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"

namespace {

// Shared temp file for entries lazily extracted from .dict.dz.
constexpr const char* DICT_TMP_FILE = "/.crosspoint/dict.tmp";

// Optional strip for older dumps that still use sort-key prefixes ("8bay", "Cbayed").
// Clean Casper packs use plain keys; this stays so third-party .idx files still match.
const char* stripSortKeyPrefix(const char* head) {
  if (!head || !head[0] || !head[1]) return head;
  const unsigned char h0 = static_cast<unsigned char>(head[0]);
  const unsigned char h1 = static_cast<unsigned char>(head[1]);
  if (std::isdigit(h0)) return head + 1;
  if (h0 >= 'A' && h0 <= 'Z' && h1 >= 'a' && h1 <= 'z') return head + 1;
  return head;
}

bool headwordEqualsTarget(const char* head, const char* target) {
  if (!head || !target) return false;
  if (StringUtils::asciiCaseCmp(head, target) == 0) return true;
  const char* stripped = stripSortKeyPrefix(head);
  return stripped != head && StringUtils::asciiCaseCmp(stripped, target) == 0;
}

// Pure inflection stubs only, e.g. "simple past of loll" with no real glosses.
// Clean packs write past forms as form-line + numbered senses ("1. to bark…");
// those must NOT be treated as stubs — otherwise "bayed" is skipped and lookup
// falls through to the full multi-homograph "bay" (water / laurel / bark).
bool definitionLooksLikeInflectionStub(const std::string& def) {
  if (def.empty() || def.size() > 160) return false;

  // Numbered senses mean this is a real entry (possibly form + glosses).
  for (size_t i = 0; i + 1 < def.size(); ++i) {
    if (def[i] == '1' && (def[i + 1] == '.' || def[i + 1] == ')')) return false;
  }

  char buf[161];
  const size_t n = std::min(def.size(), size_t(160));
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(def[i]);
    buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
  }
  buf[n] = '\0';

  static const char* kMarkers[] = {
      "simple past",           "past participle",       "present participle", "past tense",
      "third-person singular", "third person singular", "plural of",          "gerund of",
  };
  for (const char* m : kMarkers) {
    const char* p = strstr(buf, m);
    if (p != nullptr && static_cast<size_t>(p - buf) <= 80) return true;
  }
  return false;
}

// .qidx sidecar header: magic, version, sample interval, sample count, and the
// .idx file size the sidecar was built from (staleness check).
constexpr uint32_t QIDX_MAGIC = 0x58444951;  // "QIDX" little-endian
constexpr uint32_t QIDX_VERSION = 1;
constexpr size_t QIDX_HEADER_BYTES = 5 * sizeof(uint32_t);

struct QidxHeader {
  uint32_t sampleCount = 0;
  uint32_t idxFileSize = 0;
  bool valid = false;
};

QidxHeader readQidxHeader(HalFile& qidx, uint32_t sampleInterval) {
  QidxHeader header;
  uint32_t raw[5];
  if (!qidx.seekSet(0) || qidx.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return header;
  if (raw[0] != QIDX_MAGIC || raw[1] != QIDX_VERSION || raw[2] != sampleInterval) return header;
  header.sampleCount = raw[3];
  header.idxFileSize = raw[4];
  header.valid = true;
  return header;
}

bool readSampleOffset(HalFile& qidx, uint32_t sampleIndex, uint32_t* out) {
  if (!qidx.seekSet(QIDX_HEADER_BYTES + static_cast<size_t>(sampleIndex) * sizeof(uint32_t))) return false;
  return qidx.read(out, sizeof(*out)) == static_cast<int>(sizeof(*out));
}

uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Word characters for edge-trim only (legacy). Prefer normalizeLookupKey for lookups.
bool isWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }

// Spanish object clitics longest-first (ayudame → ayuda, damelo → dame).
constexpr const char* kEsClitics[] = {
    "melos", "melas", "telos", "telas", "selos", "selas", "noslos", "noslas", "noslo", "nosla",
    "selo",  "sela",  "melo",  "mela",  "telo",  "tela",  "lelo",   "lela",   "les",   "los",
    "las",   "nos",   "os",    "me",    "te",    "se",    "lo",     "la",     "le",
};

bool trySpanishCliticStrip(std::string& word, std::string* strippedClitic = nullptr) {
  const size_t n = word.size();
  if (n < 5) return false;
  for (const char* c : kEsClitics) {
    const size_t sn = strlen(c);
    if (n <= sn + 2) continue;
    if (word.compare(n - sn, sn, c) != 0) continue;
    if (strippedClitic) *strippedClitic = c;
    word.resize(n - sn);
    return true;
  }
  return false;
}

// Human-readable clitic gloss for the e-ink card (English, short).
const char* esCliticGloss(const char* clitic) {
  if (!clitic || !clitic[0]) return "";
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

// Short English object for natural phrases ("help me"). nullptr when multi-clitic.
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

bool isBareGenderOrPosToken(const std::string& low) {
  return low == "f" || low == "m" || low == "n" || low == "mf" || low == "v" || low == "adj" || low == "adv" ||
         low == "prep" || low == "conj" || low == "pron" || low == "interj" || low == "art" || low == "num" ||
         low == "noun" || low == "verb" || low == "adjective" || low == "adverb" || low == "pronoun" ||
         low == "preposition" || low == "conjunction" || low == "article" || low == "particle" || low == "misc";
}

// First short English gloss from a bilingual def — skips gender tags (f/m) and POS.
// "f\n1. help, aid\nm\n1. helper" → "help"
bool extractFirstEnglishGloss(const std::string& def, std::string& out) {
  out.clear();
  size_t i = 0;
  while (i < def.size()) {
    while (i < def.size() && (def[i] == ' ' || def[i] == '\t' || def[i] == '\r' || def[i] == '\n' || def[i] == ':')) {
      ++i;
    }
    if (i >= def.size()) break;
    size_t lineEnd = i;
    while (lineEnd < def.size() && def[lineEnd] != '\n' && def[lineEnd] != '\r') ++lineEnd;
    std::string line = def.substr(i, lineEnd - i);
    i = lineEnd;

    // Trim
    while (!line.empty() && line.back() == ' ') line.pop_back();
    size_t a = 0;
    while (a < line.size() && line[a] == ' ') ++a;
    if (a) line = line.substr(a);
    if (line.empty()) continue;
    if (line[0] == '/' || line[0] == '[') continue;  // pronunciation

    // Strip leading "1. " / "1) "
    if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) {
      size_t j = 0;
      while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
      if (j < line.size() && (line[j] == '.' || line[j] == ')')) {
        ++j;
        while (j < line.size() && line[j] == ' ') ++j;
        line = line.substr(j);
      }
    }
    if (line.empty()) continue;

    // Skip bare gender / POS tokens (f, m, v, noun, …)
    std::string low;
    low.reserve(line.size());
    for (char c : line) {
      low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (isBareGenderOrPosToken(low)) continue;

    // First gloss token up to comma / semicolon / paren
    size_t w = 0;
    while (w < line.size() && w < 40) {
      const char c = line[w];
      if (c == ',' || c == ';' || c == '.' || c == '(' || c == '[' || c == '|' || c == '/') break;
      ++w;
    }
    while (w > 0 && line[w - 1] == ' ') --w;
    if (w < 2) continue;
    out.assign(line, 0, w);

    // "to help" → "help"
    if (out.size() > 3 && out[0] == 't' && out[1] == 'o' && out[2] == ' ') {
      out.erase(0, 3);
    }
    // Drop leading article
    if (out.size() > 2 && out[0] == 'a' && out[1] == ' ') {
      out.erase(0, 2);
    } else if (out.size() > 3 && out.compare(0, 3, "an ") == 0) {
      out.erase(0, 3);
    } else if (out.size() > 4 && out.compare(0, 4, "the ") == 0) {
      out.erase(0, 4);
    }
    // At most 3 words
    int spaces = 0;
    for (size_t k = 0; k < out.size(); ++k) {
      if (out[k] == ' ') {
        ++spaces;
        if (spaces >= 3) {
          out.resize(k);
          break;
        }
      }
    }
    if (out.size() >= 2) return true;
    out.clear();
  }
  return false;
}

// Prepend "help me" style phrase when the page token was a Spanish clitic form.
// Lines start with '!' so DictionaryDefinitionActivity keeps them as an
// unnumbered preamble (not "1. help me").
void decorateCliticDefinition(const std::string& clitic, const std::string& matchedHead, std::string& definitionOut) {
  if (clitic.empty() || definitionOut.empty()) return;

  const char* obj = esCliticEnglishObject(clitic.c_str());
  const char* gloss = esCliticGloss(clitic.c_str());
  std::string verbGloss;
  const bool hasNatural = obj && extractFirstEnglishGloss(definitionOut, verbGloss);

  // Stack buffer is enough for the header; body is already heap-owned.
  char header[192];
  if (hasNatural && gloss && gloss[0]) {
    snprintf(header, sizeof(header), "!%s %s\n!%s + %s (%s)\n", verbGloss.c_str(), obj, matchedHead.c_str(),
             clitic.c_str(), gloss);
  } else if (hasNatural) {
    snprintf(header, sizeof(header), "!%s %s\n!%s + %s\n", verbGloss.c_str(), obj, matchedHead.c_str(), clitic.c_str());
  } else if (gloss && gloss[0]) {
    snprintf(header, sizeof(header), "!%s + %s (%s)\n", matchedHead.c_str(), clitic.c_str(), gloss);
  } else {
    snprintf(header, sizeof(header), "!%s + %s\n", matchedHead.c_str(), clitic.c_str());
  }
  definitionOut.insert(0, header);
}

// Fold Spanish accents for a second-chance lookup (á→a; ñ kept as ñ).
std::string foldSpanishAccents(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    const unsigned char b0 = static_cast<unsigned char>(in[i]);
    if (b0 == 0xC3 && i + 1 < in.size()) {
      unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
      char repl = 0;
      if (b1 == 0xA1 || b1 == 0x81)
        repl = 'a';
      else if (b1 == 0xA9 || b1 == 0x89)
        repl = 'e';
      else if (b1 == 0xAD || b1 == 0x8D)
        repl = 'i';
      else if (b1 == 0xB3 || b1 == 0x93)
        repl = 'o';
      else if (b1 == 0xBA || b1 == 0x9A || b1 == 0xBC || b1 == 0x9C)
        repl = 'u';
      if (repl) {
        out.push_back(repl);
        i += 2;
        continue;
      }
    }
    out.push_back(in[i++]);
  }
  return out;
}

// True when the .ifo declares 64-bit index offsets, which this reader does not
// support (only scans the first 2KB — idxoffsetbits always appears early).
bool ifoDeclares64BitOffsets(const std::string& ifoPath) {
  HalFile ifo;
  if (!Storage.openFileForRead("DICT", ifoPath, ifo)) return false;
  char buf[2048];
  const int n = ifo.read(buf, sizeof(buf) - 1);
  if (n <= 0) return false;
  buf[n] = '\0';
  const char* line = strstr(buf, "idxoffsetbits");
  if (!line) return false;
  const char* eq = strchr(line, '=');
  return eq && strtol(eq + 1, nullptr, 10) == 64;
}

}  // namespace

bool Dictionary::open(const char* folderName) {
  basePath.clear();
  std::string resolved;
  if (!DictionaryRegistry::resolveBasePath(folderName, resolved)) {
    LOG_ERR("DICT", "No dictionary found in folder '%s'", folderName ? folderName : "");
    return false;
  }

  if (!Storage.exists((resolved + ".idx").c_str())) {
    LOG_ERR("DICT", "%s.idx missing (compressed .idx.gz is not supported)", resolved.c_str());
    return false;
  }
  hasPlainDict = Storage.exists((resolved + ".dict").c_str());
  if (!hasPlainDict && !Storage.exists((resolved + ".dict.dz").c_str())) {
    LOG_ERR("DICT", "%s has no .dict or .dict.dz", resolved.c_str());
    return false;
  }
  if (ifoDeclares64BitOffsets(resolved + ".ifo")) {
    LOG_ERR("DICT", "%s uses 64-bit index offsets (unsupported)", resolved.c_str());
    return false;
  }

  basePath = std::move(resolved);
  return true;
}

bool Dictionary::needsIndex() {
  if (!isOpen()) return false;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return false;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  HalFile qidx;
  if (!Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) return true;
  const QidxHeader header = readQidxHeader(qidx, SAMPLE_INTERVAL);
  return !header.valid || header.idxFileSize != idxSize;
}

bool Dictionary::buildIndex(void (*yieldFn)(void*), void* ctx) {
  if (!isOpen()) return false;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return false;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  constexpr size_t CHUNK_BYTES = 4096;
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK_BYTES);
  if (!buf) {
    LOG_ERR("DICT", "OOM: %u byte index scan buffer", CHUNK_BYTES);
    return false;
  }

  // Stream each sample offset straight to the sidecar instead of accumulating
  // them in RAM: a large .idx would otherwise cost tens of KB of vector heap,
  // and vector growth aborts on OOM under -fno-exceptions. The header slot is
  // zero-filled until the scan succeeds, so an interrupted build leaves a file
  // readQidxHeader rejects (magic mismatch) and needsIndex() triggers a rebuild.
  const std::string qidxPath = basePath + ".qidx";
  HalFile out;
  if (!Storage.openFileForWrite("DICT", qidxPath, out)) return false;
  const auto writeU32 = [&out](uint32_t v) { return out.write(&v, sizeof(v)) == static_cast<int>(sizeof(v)); };
  const uint32_t placeholder[5] = {};
  bool ok = out.write(placeholder, sizeof(placeholder)) == sizeof(placeholder);
  uint32_t sampleCount = 0;
  if (ok) {
    ok = writeU32(0);  // entry 0 always starts at byte 0
    sampleCount = 1;
  }

  const unsigned long startMs = millis();
  uint32_t entryCount = 0;
  uint32_t pos = 0;
  uint32_t suffixLeft = 0;  // 0 while scanning a headword, else suffix bytes remaining
  uint32_t sinceYield = 0;
  while (ok && pos < idxSize) {
    const int n = idx.read(buf.get(), CHUNK_BYTES);
    if (n <= 0) {
      LOG_ERR("DICT", "Index scan read failed at %lu", static_cast<unsigned long>(pos));
      ok = false;
      break;
    }
    for (int i = 0; ok && i < n; i++) {
      if (suffixLeft == 0) {
        if (buf[i] == 0) suffixLeft = 8;
      } else if (--suffixLeft == 0) {
        entryCount++;
        const uint32_t nextEntryStart = pos + i + 1;
        if (entryCount % SAMPLE_INTERVAL == 0 && nextEntryStart < idxSize) {
          ok = writeU32(nextEntryStart);
          sampleCount++;
        }
      }
    }
    pos += n;
    sinceYield += n;
    if (yieldFn && sinceYield >= 64 * 1024) {
      sinceYield = 0;
      yieldFn(ctx);
    }
  }

  if (ok) {
    // Backpatch the now-valid header over the placeholder.
    const uint32_t header[5] = {QIDX_MAGIC, QIDX_VERSION, SAMPLE_INTERVAL, sampleCount, idxSize};
    ok = out.seekSet(0) && out.write(header, sizeof(header)) == sizeof(header);
  }
  if (!ok) {
    LOG_ERR("DICT", "Index build failed, removing %s", qidxPath.c_str());
    out.close();  // close before remove of the same path
    Storage.remove(qidxPath.c_str());
    return false;
  }

  LOG_INF("DICT", "Indexed %lu entries (%lu samples) in %lu ms", static_cast<unsigned long>(entryCount),
          static_cast<unsigned long>(sampleCount), millis() - startMs);
  return true;
}

void Dictionary::idxSeek(HalFile& idx, uint32_t absPos) {
  idx.seekSet(absPos);
  idxCursor = &idx;
  idxBufPos = 0;
  idxBufLen = 0;
}

int Dictionary::idxReadByte() {
  if (!idxCursor) return -1;
  if (idxBufPos >= idxBufLen) {
    const int n = idxCursor->read(idxBuf, static_cast<int>(IDX_BUF_SIZE));
    if (n <= 0) {
      idxBufLen = 0;
      idxBufPos = 0;
      return -1;
    }
    idxBufLen = static_cast<size_t>(n);
    idxBufPos = 0;
  }
  return idxBuf[idxBufPos++];
}

bool Dictionary::idxReadExact(uint8_t* dst, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const int ch = idxReadByte();
    if (ch < 0) return false;
    dst[i] = static_cast<uint8_t>(ch);
  }
  return true;
}

int Dictionary::readWordIntoBuffered() {
  size_t i = 0;
  while (i < sizeof(wordBuf) - 1) {
    const int ch = idxReadByte();
    if (ch < 0) return -1;
    if (ch == 0) {
      wordBuf[i] = '\0';
      return static_cast<int>(i);
    }
    wordBuf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync.
  wordBuf[sizeof(wordBuf) - 1] = '\0';
  int ch;
  do {
    ch = idxReadByte();
  } while (ch > 0);
  return static_cast<int>(sizeof(wordBuf) - 1);
}

DictLocation Dictionary::locateInOpen(HalFile& idx, uint32_t /*idxSize*/, const uint32_t* samples, uint32_t sampleCount,
                                      HalFile* qidxOrNull, const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!target || !*target) return result;

  // Bisect samples to the last sample whose headword <= target.
  // Prefer a RAM table (one sequential .qidx read). Fall back to seek-per-mid.
  uint32_t startByte = 0;
  bool limitedScan = false;  // true only when bisect landed us in a sample window
  if (sampleCount > 0) {
    uint32_t lo = 0;
    uint32_t hi = sampleCount - 1;
    bool bisectOk = true;
    while (lo < hi) {
      const uint32_t mid = (lo + hi + 1) / 2;
      uint32_t offset = 0;
      if (samples) {
        offset = samples[mid];
      } else if (!qidxOrNull || !readSampleOffset(*qidxOrNull, mid, &offset)) {
        bisectOk = false;
        break;
      }
      idxSeek(idx, offset);
      if (readWordIntoBuffered() < 0) {
        bisectOk = false;
        break;
      }
      if (StringUtils::asciiCaseCmp(wordBuf, target) <= 0) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    if (bisectOk) {
      if (samples) {
        startByte = samples[lo];
      } else if (qidxOrNull) {
        (void)readSampleOffset(*qidxOrNull, lo, &startByte);
      }
      limitedScan = true;
    }
  }

  // Linear scan from the sample start. With a good bisect, the hit (if any)
  // lies within the next SAMPLE_INTERVAL entries. Without samples, scan until
  // the sorted headword stream passes the target (still stops early on miss).
  idxSeek(idx, startByte);
  uint32_t scanned = 0;
  while (!limitedScan || scanned < SAMPLE_INTERVAL) {
    if (readWordIntoBuffered() < 0) break;
    uint8_t suffix[8];
    if (!idxReadExact(suffix, 8)) break;
    ++scanned;

    // Exact match, or Wiktionary sort-key match ("Cbayed" ↔ "bayed", "8bay" ↔ "bay").
    if (headwordEqualsTarget(wordBuf, target)) {
      result.offset = readBe32(suffix);
      result.size = readBe32(suffix + 4);
      result.found = true;
      if (matchedHeadwordOut) {
        // Display without sort-key prefix.
        *matchedHeadwordOut = stripSortKeyPrefix(wordBuf);
      }
      return result;
    }
    // Raw order still drives early-out for binary-search windows.
    const int cmp = StringUtils::asciiCaseCmp(wordBuf, target);
    if (cmp > 0) break;
  }
  return result;
}

DictLocation Dictionary::locate(const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!isOpen()) return result;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return result;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  HalFile qidx;
  uint32_t sampleCount = 0;
  std::unique_ptr<uint32_t[]> samples;
  HalFile* qidxPtr = nullptr;
  if (Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) {
    const QidxHeader header = readQidxHeader(qidx, SAMPLE_INTERVAL);
    if (header.valid && header.idxFileSize == idxSize && header.sampleCount > 0) {
      sampleCount = header.sampleCount;
      qidxPtr = &qidx;
      if (sampleCount <= MAX_RAM_SAMPLES) {
        samples = makeUniqueNoThrow<uint32_t[]>(sampleCount);
        if (samples) {
          if (!qidx.seekSet(QIDX_HEADER_BYTES) ||
              qidx.read(samples.get(), static_cast<int>(sampleCount * sizeof(uint32_t))) !=
                  static_cast<int>(sampleCount * sizeof(uint32_t))) {
            samples.reset();
          }
        }
      }
    }
  }

  return locateInOpen(idx, idxSize, samples.get(), sampleCount, samples ? nullptr : qidxPtr, target,
                      matchedHeadwordOut);
}

bool Dictionary::readDefinition(const DictLocation& location, std::string& out) {
  if (!location.found) return false;
  const uint32_t size = std::min(location.size, MAX_DEFINITION_BYTES);

  std::string path;
  uint32_t offset = 0;
  if (hasPlainDict) {
    path = basePath + ".dict";
    offset = location.offset;
  } else {
    // Ensure Casper root exists (first dict use may precede other stores).
    Storage.mkdir("/.crosspoint");
    HalFile tmp = Storage.open(DICT_TMP_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DICT", "Failed to open %s", DICT_TMP_FILE);
      return false;
    }
    if (!DictZip::extractEntry((basePath + ".dict.dz").c_str(), location.offset, size, tmp)) {
      LOG_ERR("DICT", "dictzip extraction failed for %s", basePath.c_str());
      return false;
    }
    tmp.close();  // close before reopening the same path for read
    path = DICT_TMP_FILE;
  }

  HalFile dict;
  if (!Storage.openFileForRead("DICT", path, dict)) return false;
  const uint32_t dictSize = static_cast<uint32_t>(dict.fileSize());
  if (offset > dictSize || size > dictSize - offset) {
    LOG_ERR("DICT", "Definition out of bounds (%lu+%lu > %lu)", static_cast<unsigned long>(offset),
            static_cast<unsigned long>(size), static_cast<unsigned long>(dictSize));
    return false;
  }

  // std::string growth aborts on OOM (-fno-exceptions); refuse up front unless
  // the allocation fits comfortably in the largest free block.
  if (ESP.getMaxAllocHeap() < size + 8 * 1024) {
    LOG_ERR("DICT", "Low heap for %lu byte definition", static_cast<unsigned long>(size));
    return false;
  }

  dict.seekSet(offset);
  out.assign(size, '\0');
  const int bytesRead = dict.read(&out[0], size);
  if (bytesRead < 0) {
    out.clear();
    return false;
  }
  if (static_cast<uint32_t>(bytesRead) < size) out.resize(bytesRead);
  return true;
}

std::string Dictionary::cleanWord(const char* word) {
  if (!word || !*word) return "";

  // Port of Casper/legacy normalizeWord for StarDict keys:
  // - lowercase ASCII letters
  // - keep Spanish letters (UTF-8 C3 accents + ñ)
  // - keep ASCII hyphens in compounds (well-known)
  // - strip soft hyphens, curly quotes, other punctuation, digits
  // - collapse spaces (multi-word rare on page tokens)
  constexpr size_t kMaxKey = 80;
  unsigned char out[kMaxKey];
  size_t w = 0;
  const unsigned char* r = reinterpret_cast<const unsigned char*>(word);

  while (*r && w + 4 < kMaxKey) {
    // ASCII
    if (*r < 0x80) {
      unsigned char c = *r++;
      if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
      // Letters + hyphen (compounds) + space. Apostrophes stripped so "it's" → "its"
      // and curly/straight quotes around Spanish words never stick to the key.
      if (c >= 'a' && c <= 'z') {
        out[w++] = c;
      } else if (c == '-' && w > 0 && out[w - 1] != '-' && out[w - 1] != ' ') {
        out[w++] = c;  // compound hyphen
      } else if ((c == ' ' || c == '\t') && w > 0 && out[w - 1] != ' ' && out[w - 1] != '-') {
        out[w++] = ' ';
      }
      // All other ASCII (quotes, commas, digits, underscores, …) dropped.
      continue;
    }
    // 2-byte UTF-8 (Spanish accents in Latin-1 Supplement)
    if ((*r & 0xE0) == 0xC0 && r[1] != 0) {
      const unsigned char b0 = r[0];
      unsigned char b1 = r[1];
      if (b0 == 0xC3) {
        // Uppercase Spanish letters → lowercase
        if (b1 >= 0x80 && b1 <= 0x9E) {
          if (b1 == 0x81)
            b1 = 0xA1;  // Á→á
          else if (b1 == 0x89)
            b1 = 0xA9;  // É→é
          else if (b1 == 0x8D)
            b1 = 0xAD;  // Í→í
          else if (b1 == 0x93)
            b1 = 0xB3;  // Ó→ó
          else if (b1 == 0x9A)
            b1 = 0xBA;  // Ú→ú
          else if (b1 == 0x9C)
            b1 = 0xBC;  // Ü→ü
          else if (b1 == 0x91)
            b1 = 0xB1;  // Ñ→ñ
        }
        // Keep á é í ó ú ü ñ
        if (b1 == 0xA1 || b1 == 0xA9 || b1 == 0xAD || b1 == 0xB3 || b1 == 0xBA || b1 == 0xBC || b1 == 0xB1) {
          out[w++] = b0;
          out[w++] = b1;
        }
      }
      // Soft hyphen U+00AD is C2 AD — skip (line-break hyphens in EPUB).
      // Other C2 symbols skipped.
      r += 2;
      continue;
    }
    // 3-byte: curly quotes “ ” ‘ ’, en/em dashes, bullets — strip
    if ((*r & 0xF0) == 0xE0 && r[1] && r[2]) {
      // U+2010/2011 hyphen-like → treat as ASCII hyphen if useful
      if (r[0] == 0xE2 && r[1] == 0x80 && (r[2] == 0x90 || r[2] == 0x91)) {
        if (w > 0 && out[w - 1] != '-' && out[w - 1] != ' ' && w + 1 < kMaxKey) {
          out[w++] = '-';
        }
      }
      r += 3;
      continue;
    }
    if ((*r & 0xF8) == 0xF0 && r[1] && r[2] && r[3]) {
      r += 4;
      continue;
    }
    ++r;
  }
  while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '-')) --w;
  out[w] = 0;
  if (w == 0) return "";
  return std::string(reinterpret_cast<char*>(out), w);
}

void Dictionary::stemVariants(const std::string& word, std::vector<std::string>& out) {
  out.clear();
  out.reserve(16);
  const auto add = [&out](std::string v) {
    if (v.empty()) return;
    if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(std::move(v));
  };

  // Hyphen compounds: try full form and each side (well-known → well, known).
  if (word.find('-') != std::string::npos) {
    add(word);  // with hyphens (already cleaned)
    std::string noHyphen = word;
    noHyphen.erase(std::remove(noHyphen.begin(), noHyphen.end(), '-'), noHyphen.end());
    add(noHyphen);
    size_t start = 0;
    while (start < word.size()) {
      size_t dash = word.find('-', start);
      if (dash == std::string::npos) {
        add(word.substr(start));
        break;
      }
      if (dash > start) add(word.substr(start, dash - start));
      start = dash + 1;
    }
  }

  // Spanish clitics before English stems so "ayudame" is not mangled into "ayuda me" wrongly.
  // Prefer infinitives first (ayudar → "to help") so clitic phrases compose as "help me".
  {
    std::string w = word;
    std::string clitic;
    if (trySpanishCliticStrip(w, &clitic)) {
      const std::string folded = foldSpanishAccents(w);
      const auto addInfinitives = [&add](const std::string& stem) {
        if (stem.size() < 3 || stem.back() == 'r') return;
        add(stem + "ar");
        add(stem + "er");
        add(stem + "ir");
        add(stem + "r");
      };
      addInfinitives(w);
      if (folded != w) addInfinitives(folded);
      add(w);
      add(folded);
    }
  }

  // Accent-folded original (águila → aguila) for packs without accents.
  {
    std::string folded = foldSpanishAccents(word);
    if (folded != word) add(folded);
  }

  const size_t n = word.size();
  const auto endsWith = [&word, n](const char* suffix) {
    const size_t len = strlen(suffix);
    return n > len && word.compare(n - len, len, suffix) == 0;
  };

  // English possessives / plurals / verbs (same as stock 1.5, plus -ly).
  if (endsWith("'s")) add(word.substr(0, n - 2));
  if (endsWith("\xE2\x80\x99s")) add(word.substr(0, n - 4));       // U+2019
  if (endsWith("ily") && n > 5) add(word.substr(0, n - 3) + "y");  // happily → happy
  if ((endsWith("ably") || endsWith("ibly")) && n > 6) {
    std::string v = word.substr(0, n - 1);
    v.back() = 'e';  // probably → probable
    add(v);
  }
  if (endsWith("ly") && n > 5) add(word.substr(0, n - 2));  // slowly → slow
  if (endsWith("ies")) add(word.substr(0, n - 3) + "y");
  if (endsWith("es")) add(word.substr(0, n - 2));
  if (endsWith("s") && n > 3 && word[n - 2] != 's') add(word.substr(0, n - 1));
  if (endsWith("ed")) {
    add(word.substr(0, n - 2));
    add(word.substr(0, n - 1));
    if (n >= 4 && word[n - 3] == word[n - 4]) add(word.substr(0, n - 3));
  }
  if (endsWith("ing")) {
    add(word.substr(0, n - 3));
    add(word.substr(0, n - 3) + "e");
    if (n >= 5 && word[n - 4] == word[n - 5]) add(word.substr(0, n - 4));
  }
}

bool Dictionary::lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut) {
  if (!isOpen()) return false;

  const std::string cleaned = cleanWord(word);
  if (cleaned.empty()) return false;

  // Candidates: surface form first, then light stems/clitics. Multi-word
  // selections also try collocation windows and each token. Clean packs use
  // plain keys — no sort-key prefix probing (that used to add ~36×N SD seeks).
  std::vector<std::string> tryList;
  tryList.reserve(16);
  const auto addUnique = [&tryList](std::string v) {
    if (v.empty()) return;
    if (std::find(tryList.begin(), tryList.end(), v) == tryList.end()) {
      tryList.push_back(std::move(v));
    }
  };
  const auto addWithStems = [&](const std::string& key) {
    addUnique(key);
    std::vector<std::string> variants;
    stemVariants(key, variants);
    for (auto& v : variants) addUnique(std::move(v));
  };

  addWithStems(cleaned);

  if (cleaned.find(' ') != std::string::npos) {
    std::vector<std::string> tokens;
    tokens.reserve(8);
    size_t start = 0;
    while (start < cleaned.size()) {
      while (start < cleaned.size() && cleaned[start] == ' ') ++start;
      if (start >= cleaned.size()) break;
      size_t end = cleaned.find(' ', start);
      if (end == std::string::npos) end = cleaned.size();
      if (end > start) tokens.emplace_back(cleaned.substr(start, end - start));
      start = (end < cleaned.size()) ? end + 1 : end;
    }

    // Longer collocation windows first so "por favor" beats lone "favor".
    // Use signed length to avoid size_t wrap when shrinking past 2.
    if (tokens.size() >= 2) {
      for (int len = static_cast<int>(tokens.size()) - 1; len >= 2; --len) {
        const size_t windowLen = static_cast<size_t>(len);
        for (size_t i = 0; i + windowLen <= tokens.size(); ++i) {
          std::string window = tokens[i];
          for (size_t j = 1; j < windowLen; ++j) {
            window.push_back(' ');
            window += tokens[i + j];
          }
          addUnique(std::move(window));
        }
      }
    }

    // Each word with clitic/stem expansion (ayudame → ayudar / ayuda).
    for (const auto& tok : tokens) {
      addWithStems(tok);
    }
  }

  LOG_DBG("DICT", "lookup '%s' (%u candidates)", cleaned.c_str(), static_cast<unsigned>(tryList.size()));

  // Open .idx / .qidx once for all candidates (was: reopen per candidate).
  // SdFat allows only one open file — close both before readDefinition().
  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return false;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  HalFile qidx;
  uint32_t sampleCount = 0;
  std::unique_ptr<uint32_t[]> samples;
  HalFile* qidxPtr = nullptr;
  if (Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) {
    const QidxHeader header = readQidxHeader(qidx, SAMPLE_INTERVAL);
    if (header.valid && header.idxFileSize == idxSize && header.sampleCount > 0) {
      sampleCount = header.sampleCount;
      qidxPtr = &qidx;
      // One sequential read of the sample table for bisect (avoids a seek per mid).
      if (sampleCount <= MAX_RAM_SAMPLES) {
        samples = makeUniqueNoThrow<uint32_t[]>(sampleCount);
        if (samples) {
          if (!qidx.seekSet(QIDX_HEADER_BYTES) ||
              qidx.read(samples.get(), static_cast<int>(sampleCount * sizeof(uint32_t))) !=
                  static_cast<int>(sampleCount * sizeof(uint32_t))) {
            samples.reset();
          }
        }
      }
    }
  }

  // tryList is ordered: surface form first, then stems. First non-stub hit wins
  // (clean packs give "bayed"/"lolled" real verb glosses). Stubs are only kept as
  // last resort so third-party packs still resolve something.
  struct CandHit {
    DictLocation loc;
    std::string head;
    std::string candidate;
  };
  std::vector<CandHit> hits;
  hits.reserve(6);
  for (const auto& candidate : tryList) {
    std::string head;
    DictLocation loc =
        locateInOpen(idx, idxSize, samples.get(), sampleCount, samples ? nullptr : qidxPtr, candidate.c_str(), &head);
    if (loc.found) {
      hits.push_back(CandHit{loc, std::move(head), candidate});
    }
  }
  samples.reset();
  idxCursor = nullptr;
  idx.close();
  qidx.close();

  if (hits.empty()) return false;

  CandHit stubHit{};
  std::string stubDef;
  bool haveStub = false;
  std::string hitCandidate;
  bool haveBest = false;

  for (const auto& hit : hits) {
    std::string def;
    if (!readDefinition(hit.loc, def) || def.empty()) continue;
    if (definitionLooksLikeInflectionStub(def)) {
      if (!haveStub) {
        stubHit = hit;
        stubDef = std::move(def);
        haveStub = true;
      }
      continue;
    }
    definitionOut = std::move(def);
    matchedHeadwordOut = hit.head.empty() ? stripSortKeyPrefix(hit.candidate.c_str()) : hit.head;
    if (cleaned.size() > matchedHeadwordOut.size()) matchedHeadwordOut = cleaned;
    hitCandidate = hit.candidate;
    haveBest = true;
    break;  // tryList order is intentional
  }

  if (!haveBest) {
    if (!haveStub) return false;
    definitionOut = std::move(stubDef);
    matchedHeadwordOut = stubHit.head.empty() ? stripSortKeyPrefix(stubHit.candidate.c_str()) : stubHit.head;
    if (cleaned.size() > matchedHeadwordOut.size()) matchedHeadwordOut = cleaned;
    hitCandidate = stubHit.candidate;
  }

  // "ayudame" → pack hit on ayudar/ayuda: still show "help me" above the senses.
  // For multi-word, apply clitic decoration only when the hit came from the first token.
  {
    std::string cliticStem = hitCandidate.empty() ? cleaned : hitCandidate;
    // If the hit was a multi-word collocation, use its first token for clitic check.
    const size_t sp = cliticStem.find(' ');
    if (sp != std::string::npos) cliticStem.resize(sp);
    std::string stem = cliticStem;
    std::string clitic;
    if (trySpanishCliticStrip(stem, &clitic)) {
      const std::string head = matchedHeadwordOut.empty() ? stem : matchedHeadwordOut;
      decorateCliticDefinition(clitic, head, definitionOut);
    } else {
      // Full cleaned phrase started with a clitic form even if we matched another window.
      stem = cleaned;
      const size_t sp2 = stem.find(' ');
      if (sp2 != std::string::npos) stem.resize(sp2);
      if (trySpanishCliticStrip(stem, &clitic)) {
        // Only decorate when we actually resolved that first token (or its stem).
        if (hitCandidate ==
                cleaned.substr(0, cleaned.find(' ') == std::string::npos ? cleaned.size() : cleaned.find(' ')) ||
            hitCandidate == stem || hitCandidate == stem + "ar" || hitCandidate == stem + "er" ||
            hitCandidate == stem + "ir" ||
            (!matchedHeadwordOut.empty() && StringUtils::asciiCaseCmp(matchedHeadwordOut.c_str(), stem.c_str()) == 0)) {
          decorateCliticDefinition(clitic, matchedHeadwordOut.empty() ? stem : matchedHeadwordOut, definitionOut);
        }
      }
    }
  }
  return true;
}
