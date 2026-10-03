#pragma once
#include "ChapterIr.h"
#include "../Memory/BoundedUtf8.h"
#include <HalStorage.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
namespace rivulet {
// Preserve every explicit TOC anchor, but make omitted spine sections reachable.
// Labels are learned from prepared chapter text; opening the chapter menu never
// decompresses unvisited chapters just to guess their names.
class NavigationSections {
 public:
  struct Entry {int16_t spine,toc;Entry() noexcept :spine(-1),toc(-1){} Entry(int16_t s,int16_t t) noexcept:spine(s),toc(t){}};
  template<class TocSpine> bool build(int spines,int tocCount,TocSpine getSpine){
    entries_.clear();
    if(spines<=0||spines>2048||tocCount<0||tocCount>2048||spines+tocCount>2048)return false;
    casper_memory::FallibleVector<Entry> explicitEntries;
    if(!explicitEntries.reserve(tocCount)||!entries_.reserve(spines+tocCount))return false;
    for(int t=0;t<tocCount;++t)if(!explicitEntries.push_back({int16_t(getSpine(t)),int16_t(t)}))return false;
    for(int s=0;s<spines;++s){bool found=false;
      for(const auto& e:explicitEntries)if(e.spine==s){if(!entries_.push_back(e))return false;found=true;}
      if(!found&&!entries_.push_back({int16_t(s),-1}))return false;
    }
    // Unresolved publisher links remain visible/cancel-safe, not silently lost.
    for(const auto&e:explicitEntries)if(e.spine<0||e.spine>=spines)if(!entries_.push_back(e))return false;
    return !entries_.failed();
  }
  size_t size()const{return entries_.size();}
  Entry at(size_t i)const{return i<entries_.size()?entries_[i]:Entry{};}
 private:casper_memory::FallibleVector<Entry> entries_;
};
namespace sectionlabel {
inline bool path(const char*dir,int spine,char*dst,size_t cap){
  if(!dir||!*dir||spine<0)return false;int n=std::snprintf(dst,cap,"%s/title-v%u-s%d.rvt",dir,kIrFormatVersion,spine);return n>0&&size_t(n)<cap;
}
inline uint32_t hash(const char*s,size_t n){uint32_t h=2166136261u;while(n--){h^=uint8_t(*s++);h*=16777619u;}return h;}
inline std::string read(const char*dir,int spine){
  char name[256];if(!path(dir,spine,name,sizeof(name)))return {};
  HalFile f;if(!Storage.openFileForRead("RVTITLE",name,f))return {};
  uint8_t h[9]{};char s[128]{};
  if(f.read(h,sizeof(h))!=sizeof(h)||std::memcmp(h,"RVTL",4)||!h[4]||h[4]>=sizeof(s)||f.size()!=sizeof(h)+h[4]||f.read(s,h[4])!=h[4])return {};
  uint32_t proof=uint32_t(h[5])|uint32_t(h[6])<<8|uint32_t(h[7])<<16|uint32_t(h[8])<<24;
  if(hash(s,h[4])!=proof)return {};return {s,h[4]};
}
inline bool save(const char*dir,int spine,const ChapterIr& ch){
  char name[256];if(ch.failed()||!path(dir,spine,name,sizeof(name)))return false;
  if(!read(dir,spine).empty())return true;
  for(size_t bi=0;bi<std::min<size_t>(ch.blockCount(),12);++bi){const auto b=ch.blocks()[bi];
    const bool heading=b.kind>=BlockKind::Heading1&&b.kind<=BlockKind::Heading6;
    if((!heading&&b.align!=Align::Center)||b.kind==BlockKind::Image||!b.runCount)continue;
    char s[128]{};size_t length=0;bool over=false;
    for(size_t r=b.runBegin;r<size_t(b.runBegin)+b.runCount;++r){const auto run=ch.runs()[r];
      if(run.textLen>127-length){over=true;break;}std::memcpy(s+length,ch.runText(run),run.textLen);length+=run.textLen;
    }
    if(ch.failed()||over||!length)continue;
    // Only short actual heading text: never relabel arbitrary body prose.
    size_t first=0;while(first<length&&uint8_t(s[first])<=32)++first;
    while(length>first&&uint8_t(s[length-1])<=32)--length;
    if(length==first)continue;length-=first;std::memmove(s,s+first,length);
    uint8_t h[9]={'R','V','T','L',uint8_t(length),0,0,0,0};auto proof=hash(s,length);for(int i=0;i<4;++i)h[5+i]=uint8_t(proof>>(8*i));
    char tmp[272];int n=std::snprintf(tmp,sizeof(tmp),"%s.tmp",name);if(n<=0||size_t(n)>=sizeof(tmp))return false;
    HalFile f;if(!Storage.openFileForWrite("RVTITLE",tmp,f))return false;
    bool ok=f.write(h,sizeof(h))==sizeof(h)&&f.write(s,length)==length;f.flush();f.close();
    if(!ok){Storage.remove(tmp);return false;}Storage.remove(name);if(!Storage.rename(tmp,name)){Storage.remove(tmp);return false;}return true;
  }return false;
}
}
}
