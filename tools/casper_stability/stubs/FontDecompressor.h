#pragma once
#include "EpdFontFamily.h"
#include <string>
#include <vector>
struct FontPrewarmCall { const EpdFontData* data; std::string text; };
class FontDecompressor {
 public:
  std::vector<FontPrewarmCall> calls;
  void clearCache() {}
  int prewarmCache(const EpdFontData* d,const char* s) { calls.push_back({d,s});return 0; }
  void logStats(const char*) {} void resetStats() {}
};
