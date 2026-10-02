#pragma once
#include <HalStorage.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "RivuletEngine.h"
namespace rivulet {
// User state, not a layout cache. Two checksummed slots keep the previous
// committed anchor readable if power is lost while the next slot is written.
struct ProgressAnchor {
  uint64_t source=0;
  uint32_t sequence=0,spine=0,page=0,textOffset=UINT32_MAX,context=0;
  IrCursor cursor{};
  uint16_t format=kIrFormatVersion;
  static uint32_t hash(const void* data,size_t n){uint32_t h=2166136261U;const auto*p=static_cast<const uint8_t*>(data);while(n--){h^=*p++;h*=16777619U;}return h;}
  static bool capture(const RivuletEngine& engine,uint64_t source,int spine,ProgressAnchor& a){
    if(!source || spine<0 || !engine.hasCurrentStartCursor() || engine.chapter().failed())return false;
    a={};a.source=source;a.spine=spine;a.page=engine.currentPage();a.cursor=engine.currentStartCursor();
    const auto& chapter=engine.chapter();
    if(a.cursor.blockIndex>=chapter.blockCount())return false;
    const Block b=chapter.blocks()[a.cursor.blockIndex];
    if(a.cursor.runIndex<chapter.runs().size() && a.cursor.runIndex<uint32_t(b.runBegin)+b.runCount){
      const Run run=chapter.runs()[a.cursor.runIndex];
      if(a.cursor.byteInRun>run.textLen)return false;
      a.textOffset=run.textOff+a.cursor.byteInRun;
      a.context=hash(chapter.runText(run)+a.cursor.byteInRun,std::min<size_t>(16,run.textLen-a.cursor.byteInRun));
    }
    return !chapter.failed();
  }
  bool resolve(const ChapterIr& chapter,IrCursor& out)const{
    if(chapter.failed())return false;
    if(format==kIrFormatVersion && cursor.blockIndex<chapter.blockCount()){
      const Block b=chapter.blocks()[cursor.blockIndex];
      if(cursor.runIndex>=b.runBegin && cursor.runIndex<=uint32_t(b.runBegin)+b.runCount){
        if(textOffset==UINT32_MAX && cursor.byteInRun==0){out=cursor;return true;}
        if(cursor.runIndex<chapter.runs().size()){
          const Run run=chapter.runs()[cursor.runIndex];
          if(cursor.byteInRun<=run.textLen && textOffset==run.textOff+cursor.byteInRun &&
             context==hash(chapter.runText(run)+cursor.byteInRun,std::min<size_t>(16,run.textLen-cursor.byteInRun))){out=cursor;return !chapter.failed();}
        }
      }
    }
    // A parser/run-splitting upgrade can change run indices without changing
    // source text offsets. Require the saved context before accepting a remap.
    if(textOffset==UINT32_MAX)return false;
    for(size_t bi=0;bi<chapter.blockCount();++bi){const Block b=chapter.blocks()[bi];
      for(size_t ri=b.runBegin;ri<size_t(b.runBegin)+b.runCount;++ri){const Run run=chapter.runs()[ri];
        if(textOffset>=run.textOff && textOffset-run.textOff<run.textLen){
          const size_t offset=textOffset-run.textOff;
          if(context==hash(chapter.runText(run)+offset,std::min<size_t>(16,run.textLen-offset))){out={uint16_t(bi),uint16_t(ri),uint16_t(offset)};return !chapter.failed();}
        }
      }
    }return false;
  }
  static bool load(const std::string& dir,uint64_t source,ProgressAnchor& out){
    ProgressAnchor a,b;const bool va=readSlot(dir,0,a)&&a.source==source,vb=readSlot(dir,1,b)&&b.source==source;
    if(!va&&!vb)return false;out=va&&(!vb||newer(a.sequence,b.sequence))?a:b;return true;
  }
  static bool save(const std::string& dir,ProgressAnchor& value){
    if(dir.empty()||!value.source)return false;
    ProgressAnchor a,b;const bool va=readSlot(dir,0,a),vb=readSlot(dir,1,b);
    const bool latestA=va&&(!vb||newer(a.sequence,b.sequence));
    const int slot=latestA?1:0;const uint32_t seq=latestA?a.sequence:(vb?b.sequence:0);
    value.sequence=seq+1;uint8_t bytes[64]{};encode(value,bytes);
    char path[256];if(!slotPath(dir,slot,path,sizeof(path)))return false;
    HalFile f;if(!Storage.openFileForWrite("ANCHOR",path,f))return false;
    const bool written=f.write(bytes,sizeof(bytes))==sizeof(bytes);f.flush();f.close();
    if(!written)return false;
    ProgressAnchor check;return readSlot(dir,slot,check)&&check.sequence==value.sequence&&check.source==value.source;
  }
 private:
  static bool newer(uint32_t a,uint32_t b){return static_cast<int32_t>(a-b)>0;}
  static uint32_t get(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
  static void put(uint8_t*p,uint32_t n){for(int i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static bool slotPath(const std::string&dir,int slot,char*out,size_t n){return std::snprintf(out,n,"%s/progress.rva%d",dir.c_str(),slot)<int(n);}
  static void encode(const ProgressAnchor&a,uint8_t*out){
    std::memcpy(out,"RVAN",4);put(out+4,1);put(out+8,uint32_t(a.source));put(out+12,uint32_t(a.source>>32));
    put(out+16,a.sequence);put(out+20,a.spine);put(out+24,a.page);put(out+28,a.textOffset);put(out+32,a.context);
    put(out+36,a.cursor.blockIndex);put(out+40,a.cursor.runIndex);put(out+44,a.cursor.byteInRun);put(out+48,a.format);
    put(out+60,hash(out,60));
  }
  static bool readSlot(const std::string&dir,int slot,ProgressAnchor&out){
    char path[256];if(!slotPath(dir,slot,path,sizeof(path)))return false;
    HalFile f;if(!Storage.openFileForRead("ANCHOR",path,f)||f.size()!=64)return false;
    uint8_t b[64];if(f.read(b,sizeof(b))!=sizeof(b)||std::memcmp(b,"RVAN",4)||get(b+4)!=1||get(b+60)!=hash(b,60))return false;
    if(get(b+20)>65534||get(b+24)>1000000||get(b+36)>65534||get(b+40)>65534||get(b+44)>65535||get(b+48)>65535)return false;
    out.source=uint64_t(get(b+8))|(uint64_t(get(b+12))<<32);out.sequence=get(b+16);out.spine=get(b+20);out.page=get(b+24);
    out.textOffset=get(b+28);out.context=get(b+32);out.cursor={uint16_t(get(b+36)),uint16_t(get(b+40)),uint16_t(get(b+44))};out.format=get(b+48);return true;
  }
};
} // namespace rivulet
