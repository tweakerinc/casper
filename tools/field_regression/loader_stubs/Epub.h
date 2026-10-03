#pragma once
// ZIP transport is replaced below the chapter loader, not the parser/storage.
// Inputs come from a private unpacked EPUB or synthetic fixture directory.
#include <string>
#include <vector>
#include <cstdio>
#include <Print.h>
class Epub {
 public:
  std::string root;
  std::vector<std::string> spines;
  unsigned transfers=0;
  bool failTransfer=false;
  struct Item {std::string href;};
  int getSpineItemsCount() const {return int(spines.size());}
  Item getSpineItem(int i) const {return {spines.at(i)};}
  bool getItemSize(const std::string& href,size_t* n) const {
    FILE* f=std::fopen((root+"/"+href).c_str(),"rb");if(!f)return false;
    std::fseek(f,0,SEEK_END);long size=std::ftell(f);std::fclose(f);if(size<0)return false;*n=size_t(size);return true;
  }
  bool readItemContentsToStream(const std::string& href,Print& sink,size_t chunk){
    ++transfers;if(failTransfer)return false;
    FILE* f=std::fopen((root+"/"+href).c_str(),"rb");if(!f)return false;
    std::vector<uint8_t> b(chunk);size_t n;bool ok=true;
    while((n=std::fread(b.data(),1,b.size(),f))){if(sink.write(b.data(),n)!=n){ok=false;break;}}
    ok=ok&&!std::ferror(f);std::fclose(f);return ok;
  }
};
