#pragma once
#include <cstdint>
#include <string>
#include <vector>
// Host capture sink only; production XML parsers and path utilities are compiled.
class BookMetadataCache {
 public:
  struct Entry { std::string title, href, anchor; uint8_t level; };
  std::vector<Entry> entries;
  void createTocEntry(const std::string& t, const std::string& h, const std::string& a, uint8_t l) {
    entries.push_back({t,h,a,l});
  }
};
