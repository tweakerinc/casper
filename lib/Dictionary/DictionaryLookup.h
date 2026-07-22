#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <cstddef>

// Offline dictionary lookup from SD (English / Spanish packs).
// Files (any subset may be present):
//   /.crosspoint/dict/en.cxdict      — English definitions
//   /.crosspoint/dict/es.cxdict      — Spanish definitions
//   /.crosspoint/dict/en-es.cxdict   — English → Spanish
//   /.crosspoint/dict/es-en.cxdict   — Spanish → English
// Format: see scripts/build_en_dict.py
// Designed for ESP32-C3: binary search on disk, no full-file load.
class DictionaryLookup {
 public:
  static constexpr char kDir[] = "/.crosspoint/dict";
  static constexpr char kPathEn[] = "/.crosspoint/dict/en.cxdict";
  static constexpr char kPathEs[] = "/.crosspoint/dict/es.cxdict";
  static constexpr char kPathEnEs[] = "/.crosspoint/dict/en-es.cxdict";
  static constexpr char kPathEsEn[] = "/.crosspoint/dict/es-en.cxdict";
  static constexpr char kDefaultPath[] = "/.crosspoint/dict/en.cxdict";
  static constexpr size_t kMaxKeyLen = 40;
  // Room for multi-pack hits + form-of expansion on a small e-ink popup.
  static constexpr size_t kMaxDefinitionLen = 1024;
  static constexpr size_t kMaxLangLabelLen = 24;

  enum class Lang : uint8_t {
    Auto = 0,  // merge hits across installed packs
    En = 1,
    Es = 2,
    EnEs = 3,
    EsEn = 4,
  };

  // True if at least one known dictionary file is present and valid.
  static bool anyAvailable();

  // Opens the dictionary if present. Safe to call repeatedly.
  static bool available(const char* path = kDefaultPath);

  // Open installed packs once for a dictionary UI session (keeps SD handles open
  // to avoid re-open + seek thrash on every word). Pair with endSession().
  static void beginSession();
  static void endSession();
  static bool sessionActive();

  // Looks up word in a specific file. Returns true if found.
  // Strips punctuation/quotes, expands "plural of X" style glosses when possible.
  static bool lookup(const char* word, char* outDef, size_t outDefLen, const char* path = kDefaultPath);

  // Looks up across installed packs. Auto mode can return multiple sections
  // (e.g. EN + ES->EN). outLangLabel becomes e.g. "EN+ES->EN".
  static bool lookupAuto(const char* word, char* outDef, size_t outDefLen, char* outLangLabel = nullptr,
                         size_t outLangLabelLen = 0, Lang prefer = Lang::Auto);

  // Normalizes in-place for lookup: lowercase, keep letters (incl. Spanish accents),
  // ASCII hyphens and single spaces (multi-word / compounds). Strips quotes,
  // other punctuation, digits, and symbols. Returns false if nothing usable remains.
  static bool normalizeWord(char* word, size_t cap);
};
