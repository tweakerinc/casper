#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  uint32_t offset = 0;  // byte offset in .dict data
  uint32_t size = 0;    // byte length in .dict data
  bool found = false;
};

// Slim StarDict reader: exact-match lookup with a mini stemming fallback.
//
// Expects /dictionaries/<folder>/<stem>.idx (uncompressed) plus <stem>.dict or
// <stem>.dict.dz. Lookups binary-search a lazily built sampled-offset sidecar
// (<stem>.qidx, byte offset of every SAMPLE_INTERVAL-th .idx entry), then
// linear-scan at most SAMPLE_INTERVAL entries. Everything streams from SD; no
// full index is held in RAM. .idx scanning uses a small sequential buffer so
// lookups are not limited by single-byte SD reads.
class Dictionary {
 public:
  // Resolve the dictionary folder and validate its files. Rejects
  // dictionaries with 64-bit index offsets (idxoffsetbits=64 in .ifo).
  bool open(const char* folderName);
  bool isOpen() const { return !basePath.empty(); }

  // True when the .qidx sidecar is missing or stale — call buildIndex() first
  // so the UI can show an "Indexing…" message for the slow first pass.
  bool needsIndex();

  // One streaming pass over .idx writing the .qidx sidecar. yieldFn (optional)
  // is called every ~64KB consumed to feed the watchdog / repaint the UI.
  bool buildIndex(void (*yieldFn)(void*) = nullptr, void* ctx = nullptr);

  // Clean the word, then look up with a short candidate list: surface form,
  // English stems, Spanish clitics, multi-word windows. Prefers non-stub defs
  // in try-order (clean packs put past forms on their own headwords).
  bool lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut);

  // Normalize a raw page token into a lookup key (see .cpp for rules).
  static std::string cleanWord(const char* word);

  // Cap definition text held in RAM. The definition card shows ≤64 wrapped lines;
  // 16 KB is enough for display and avoids a large heap peak during cascade lookup.
  static constexpr uint32_t MAX_DEFINITION_BYTES = 16 * 1024;

 private:
  // Smaller interval → shorter linear scan after the sample bisect. .qidx lives
  // on SD (not RAM); header embeds this value so old sidecars auto-rebuild.
  static constexpr uint32_t SAMPLE_INTERVAL = 64;
  // Cap for optionally loading the sample table into RAM for the bisect.
  // 4096 * 4 = 16 KB temporary heap during locate only.
  static constexpr uint32_t MAX_RAM_SAMPLES = 4096;
  // Sequential read window for .idx headwords / suffixes.
  static constexpr size_t IDX_BUF_SIZE = 512;

  DictLocation locate(const char* target, std::string* matchedHeadwordOut);
  DictLocation locateInOpen(HalFile& idx, uint32_t idxSize, const uint32_t* samples, uint32_t sampleCount,
                            HalFile* qidxOrNull, const char* target, std::string* matchedHeadwordOut);
  bool readDefinition(const DictLocation& location, std::string& out);
  static void stemVariants(const std::string& word, std::vector<std::string>& out);

  std::string basePath;  // "/dictionaries/<folder>/<stem>", empty when not open
  bool hasPlainDict = false;

  // Shared scan buffer: lookups are single-threaded and this avoids a
  // 256-byte array on the stack of every locate() call.
  char wordBuf[256] = {};

  // Buffered sequential .idx reader (invalidated on every seek).
  uint8_t idxBuf[IDX_BUF_SIZE] = {};
  size_t idxBufPos = 0;
  size_t idxBufLen = 0;
  HalFile* idxCursor = nullptr;

  void idxSeek(HalFile& idx, uint32_t absPos);
  int idxReadByte();
  bool idxReadExact(uint8_t* dst, size_t n);
  int readWordIntoBuffered();
};
