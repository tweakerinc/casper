#pragma once
#include <HalStorage.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
namespace rivulet {
// Hash ZIP central-directory metadata (including content CRCs), not megabytes
// of compressed book data. Same-path/same-size book replacement invalidates IR.
inline bool sourceFingerprint(const char* path,uint64_t& result){
  HalFile f;if(!Storage.openFileForRead("RVID",path,f))return false;
  const size_t size=f.size();if(size<22||size>UINT32_MAX)return false;
  auto le16=[](const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8;};
  auto le32=[](const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
  uint8_t buf[1024];size_t end=size;const size_t floor=size>65557?size-65557:0;
  while(end>floor){const size_t start=end-floor>sizeof(buf)?end-sizeof(buf):floor;const size_t n=end-start;
    if(!f.seek(start)||f.read(buf,n)!=int(n))return false;
    for(size_t i=n>=22?n-22+1:0;i>0;){--i;
      if(std::memcmp(buf+i,"PK\005\006",4)!=0)continue;
      const auto*e=buf+i;if(start+i+22+le16(e+20)!=size||le16(e+4)||le16(e+6))continue;
      const uint32_t bytes=le32(e+12),offset=le32(e+16);
      if(offset>start+i||bytes>start+i-offset||le16(e+10)==65535)return false;
      uint64_t h=14695981039346656037ULL;
      auto hash=[&](const uint8_t*p,size_t len){while(len--){h^=*p++;h*=1099511628211ULL;}};
      hash(e,22);if(!f.seek(offset))return false;size_t left=bytes;
      while(left){size_t k=std::min(left,sizeof(buf));if(f.read(buf,k)!=int(k))return false;hash(buf,k);left-=k;}
      result=h;return true;
    }
    if(start==floor)break;end=start+21; // EOCD header may straddle a read boundary.
  }return false;
}
inline std::string sourceCacheDirectory(const std::string& root,const std::string& book,uint64_t* fingerprint=nullptr){
  uint64_t id=0;if(!sourceFingerprint(book.c_str(),id))return {};
  if(fingerprint)*fingerprint=id;
  char suffix[32];std::snprintf(suffix,sizeof(suffix),"/src-%08lx%08lx",static_cast<unsigned long>(id>>32),static_cast<unsigned long>(static_cast<uint32_t>(id)));
  return root+suffix;
}
} // namespace rivulet
