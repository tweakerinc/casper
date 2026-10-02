#pragma once
#include <string>
#include <vector>
class Hyphenator {
 public:
  struct BreakInfo { size_t byteOffset; bool requiresInsertedHyphen; };
  static std::vector<BreakInfo> breakOffsets(const std::string& s, bool) {
    std::vector<BreakInfo> out;
    for(size_t i=0;i<s.size();++i) if(s[i]=='-' && i+1<s.size()) out.push_back({i+1,false});
    return out;
  }
};
