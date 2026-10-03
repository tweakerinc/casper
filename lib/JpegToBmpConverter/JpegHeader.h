#pragma once
#include <cstdint>
#include <cstddef>

namespace coverdecode {
struct JpegHeader { int width=0,height=0; bool progressive=false; };
// Inspect SOF before invoking JPEGDEC. Progressive streams may define only a
// subset of Huffman tables in their first scan; the small baseline decoder's
// table builder must not see that path. No entropy data is decoded here.
template<class File> bool readJpegHeader(File& f,JpegHeader& header,void (*service)()=nullptr) {
  header={};if(!f||!f.seek(0))return false;
  uint8_t b[8];if(f.read(b,2)!=2||b[0]!=0xff||b[1]!=0xd8)return false;
  for(unsigned segments=0;segments<1024;++segments){
    if(f.read(b,1)!=1||b[0]!=0xff)return false;
    do {if(f.read(b,1)!=1)return false;} while(b[0]==0xff);
    const unsigned marker=b[0];
    if(marker==0xd9||marker==0xda||marker==0)return false;
    if(marker==0x01||(marker>=0xd0&&marker<=0xd8))continue;
    if(f.read(b,2)!=2)return false;
    const size_t length=(size_t(b[0])<<8)|b[1];if(length<2)return false;
    const size_t pos=f.position(),size=f.size();if(pos>size||length-2>size-pos)return false;
    if(marker==0xc0||marker==0xc1||marker==0xc2){
      if(length<8||f.read(b,6)!=6||b[0]!=8)return false;
      header.height=(int(b[1])<<8)|b[2];header.width=(int(b[3])<<8)|b[4];header.progressive=marker==0xc2;
      return header.width>0&&header.height>0&&(b[5]==1||b[5]==3)&&f.seek(0);
    }
    if(!f.seek(pos+length-2))return false;
    if(service)service();
  }
  return false;
}
} // namespace coverdecode
