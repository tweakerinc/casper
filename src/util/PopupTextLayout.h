#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

// Fixed storage: error/status messages must remain drawable under heap pressure.
// Lines refer to the caller's UTF-8 message; a single reusable buffer is passed
// to the existing renderer. Never split inside a UTF-8 code point.
namespace popuptext {
constexpr size_t kLineBytes = 256;
constexpr size_t kMaxLines = 8;
struct Line { size_t offset=0, bytes=0; int width=0; bool ellipsis=false; };
struct Layout { std::array<Line,kMaxLines> lines{}; size_t count=0; int width=0; };
inline size_t cpBytes(const char* p) {
  const auto c=static_cast<unsigned char>(*p);
  const size_t n=c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
  for(size_t i=1;i<n;++i)if(!p[i]||(static_cast<unsigned char>(p[i])&0xc0)!=0x80)return 1;
  return n;
}
inline size_t previousCp(const char* p,size_t n) {
  if(!n)return 0;
  --n;while(n&&(static_cast<unsigned char>(p[n])&0xc0)==0x80)--n;return n;
}
inline void copyLine(const char* text,const Line& line,char (&buffer)[kLineBytes]) {
  const size_t n=std::min(line.bytes,kLineBytes-4);
  std::memcpy(buffer,text+line.offset,n);
  if(line.ellipsis){std::memcpy(buffer+n,"...",3);buffer[n+3]=0;}
  else buffer[n]=0;
}
template<class Measure>
Layout wrap(const char* text,int maxWidth,size_t maxLines,Measure measure) {
  Layout out;
  if(!text||maxWidth<=0||maxLines==0)return out;
  maxLines=std::min(maxLines,kMaxLines);
  size_t pos=0;char buffer[kLineBytes]{};
  while(text[pos]&&out.count<maxLines) {
    while(text[pos]==' '||text[pos]=='\t'||text[pos]=='\r'||text[pos]=='\n')++pos;
    if(!text[pos])break;
    const size_t start=pos;size_t fit=0,lastSpace=0;int fitWidth=0;
    while(text[pos]&&text[pos]!='\r'&&text[pos]!='\n') {
      const size_t n=cpBytes(text+pos),bytes=pos-start+n;
      if(bytes>kLineBytes-4)break;
      std::memcpy(buffer,text+start,bytes);buffer[bytes]=0;
      const int w=measure(buffer);if(w>maxWidth)break;
      pos+=n;fit=bytes;fitWidth=w;
      if(text[pos-1]==' '||text[pos-1]=='\t')lastSpace=fit;
    }
    if(!fit) {
      // Even one glyph does not fit. Consume it without an out-of-bounds draw.
      pos=start+cpBytes(text+start);continue;
    }
    if(text[pos]&&text[pos]!='\r'&&text[pos]!='\n'&&lastSpace)fit=lastSpace;
    pos=start+fit;
    while(fit&&(text[start+fit-1]==' '||text[start+fit-1]=='\t'))--fit;
    Line line{start,fit,fitWidth,false};copyLine(text,line,buffer);line.width=measure(buffer);
    size_t rest=pos;while(text[rest]==' '||text[rest]=='\t'||text[rest]=='\r'||text[rest]=='\n')++rest;
    if(out.count+1==maxLines&&text[rest]) {
      line.ellipsis=true;copyLine(text,line,buffer);
      while(line.bytes&&measure(buffer)>maxWidth){line.bytes=previousCp(text+start,line.bytes);copyLine(text,line,buffer);}
      if(measure(buffer)>maxWidth)line.ellipsis=false;
      copyLine(text,line,buffer);line.width=measure(buffer);
    }
    out.lines[out.count++]=line;out.width=std::max(out.width,line.width);
  }
  return out;
}
} // namespace popuptext
