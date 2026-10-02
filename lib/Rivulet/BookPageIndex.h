#pragma once
#include <HalStorage.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "IrFormat.h"

namespace rivulet {
// Book-wide exact counts live on SD, not in a chapter-count-sized RAM vector.
// A torn row is unknown, never a guessed/complete count. Layout identity is
// explicit; a different font, viewport or IR generation uses a separate file.
class BookPageIndex {
 public:
  bool open(const char* dir, const RenderKey& key, uint32_t chapters) {
    close();
    if (!dir || !chapters || chapters>65534) return false;
    uint32_t sig=2166136261U;
    hash(sig,&key.fontId,sizeof(key.fontId)); hash(sig,&key.viewportW,sizeof(key.viewportW));
    hash(sig,&key.viewportH,sizeof(key.viewportH)); hash(sig,&key.marginL,1); hash(sig,&key.marginR,1);
    hash(sig,&key.marginT,1); hash(sig,&key.marginB,1); hash(sig,&key.lineCompressionQ8,2);
    hash(sig,&key.flags,1); hash(sig,&key.pad,1); hash(sig,&kIrFormatVersion,2); hash(sig,&chapters,4);
    if(std::snprintf(path_,sizeof(path_),"%s/book-%08lx.rvbi",dir,static_cast<unsigned long>(sig))>=int(sizeof(path_)))return false;
    sig_=sig; chapters_=chapters;
    file_=Storage.open(path_,O_RDWR);
    uint8_t head[32]{};
    if(file_ && file_.size()==32+8*chapters && file_.read(head,sizeof(head))==sizeof(head) &&
       std::memcmp(head,"RVBI",4)==0 && get(head+4)==1 && get(head+8)==sig && get(head+12)==chapters &&
       std::memcmp(head+16,&key,sizeof(key))==0) {
      valid_=true; return scan(0);
    }
    file_.close();
    char temp[256];if(std::snprintf(temp,sizeof(temp),"%s.tmp",path_)>=int(sizeof(temp)))return false;
    HalFile w;if(!Storage.openFileForWrite("BINDEX",temp,w))return false;
    std::memcpy(head,"RVBI",4);put(head+4,1);put(head+8,sig);put(head+12,chapters);
    static_assert(sizeof(RenderKey)<=16);std::memcpy(head+16,&key,sizeof(key));
    bool ok=w.write(head,sizeof(head))==sizeof(head);
    uint8_t zeros[256]{};size_t left=chapters*8;
    while(ok&&left){const size_t n=std::min(left,sizeof(zeros));ok=w.write(zeros,n)==n;left-=n;}
    w.flush();w.close();
    if(!ok){Storage.remove(temp);return false;}
    Storage.remove(path_);if(!Storage.rename(temp,path_)){Storage.remove(temp);return false;}
    file_=Storage.open(path_,O_RDWR);valid_=bool(file_);return valid_&&scan(0);
  }
  void close(){file_.close();valid_=false;chapters_=known_=total_=prefix_=focus_=0;prefixExact_=false;}
  bool valid()const{return valid_;}
  bool matches(const RenderKey& k)const {
    if(!valid_)return false;
    uint8_t head[32]{};return file_.seek(0)&&file_.read(head,32)==32&&std::memcmp(head+16,&k,sizeof(k))==0;
  }
  bool read(uint32_t spine,uint32_t& pages)const {
    if(!valid_||spine>=chapters_)return false;
    uint8_t row[8];if(!file_.seek(32+8*spine)||file_.read(row,8)!=8)return false;
    pages=get(row);return pages<=1000000&&get(row+4)==proof(spine,pages);
  }
  bool record(uint32_t spine,uint32_t pages){
    if(!valid_||spine>=chapters_||pages>1000000)return false;
    uint32_t old;if(read(spine,old)&&old==pages)return true;
    uint8_t row[8];put(row,pages);put(row+4,proof(spine,pages));
    if(!file_.seek(32+8*spine)||file_.write(row,8)!=8){valid_=false;return false;}
    file_.flush();return scan(focus_);
  }
  bool forget(uint32_t spine){
    if(!valid_||spine>=chapters_)return false;uint8_t row[8]{};
    if(!file_.seek(32+8*spine)||file_.write(row,8)!=8){valid_=false;return false;}
    file_.flush();return scan(focus_);
  }
  bool focus(uint32_t spine){return valid_&&scan(spine);}
  bool complete()const{return valid_&&known_==chapters_;}
  bool prefixExact()const{return valid_&&prefixExact_;}
  uint32_t total()const{return total_;}
  uint32_t prefix()const{return prefix_;}
  uint32_t known()const{return known_;}
 private:
  static uint32_t get(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
  static void put(uint8_t*p,uint32_t n){for(int i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static void hash(uint32_t& h,const void* ptr,size_t n){const auto*p=static_cast<const uint8_t*>(ptr);while(n--){h^=*p++;h*=16777619U;}}
  uint32_t proof(uint32_t spine,uint32_t pages)const{uint32_t h=sig_;hash(h,&spine,4);hash(h,&pages,4);return h?h:1;}
  bool scan(uint32_t focus){
    focus_=focus;known_=total_=prefix_=0;prefixExact_=true;
    if(!file_.seek(32)){valid_=false;return false;}
    uint8_t rows[256];uint32_t index=0;
    while(index<chapters_){const uint32_t n=std::min<uint32_t>(32,chapters_-index);
      if(file_.read(rows,n*8)!=int(n*8)){valid_=false;return false;}
      for(uint32_t j=0;j<n;++j,++index){const uint32_t pages=get(rows+j*8);
        if(pages<=1000000&&get(rows+j*8+4)==proof(index,pages)&&pages<=UINT32_MAX-total_){
          ++known_;total_+=pages;if(index<focus_)prefix_+=pages;
        }else if(index<focus_)prefixExact_=false;
      }
    }return true;
  }
  mutable HalFile file_;char path_[240]{};uint32_t sig_=0,chapters_=0,known_=0,total_=0,prefix_=0,focus_=0;
  bool valid_=false,prefixExact_=false;
};
} // namespace rivulet
