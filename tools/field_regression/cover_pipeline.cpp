// Exercise the production converter and storage adapter. Input JPEG is supplied
// externally: no user's EPUB or cover is stored in the repository.
#include <Esp.h>
#include <JpegToBmpConverter.h>
#include <HalStorage.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cassert>
EspStub ESP;
class Output : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t write(uint8_t b) override {bytes.push_back(b);return 1;}
  size_t write(const uint8_t* p,size_t n) override {bytes.insert(bytes.end(),p,p+n);return n;}
};
static uint32_t u32(const std::vector<uint8_t>& b,size_t i){return uint32_t(b.at(i))|uint32_t(b.at(i+1))<<8|uint32_t(b.at(i+2))<<16|uint32_t(b.at(i+3))<<24;}
static void validate(const Output& out,int maxW,int maxH){
  const auto& b=out.bytes;assert(b.size()>=70&&b[0]=='B'&&b[1]=='M');
  const int w=int(u32(b,18)),h=-int32_t(u32(b,22));
  assert(w>0&&h>0&&w<=maxW&&h<=maxH&&b[28]==2);
  assert(u32(b,2)==b.size()&&u32(b,10)==70);
  const size_t row=((size_t(w)*2+31)/32)*4;
  assert(b.size()==70+row*size_t(h));
  unsigned counts[4]{};for(int y=0;y<h;++y)for(int x=0;x<w;++x)++counts[(b[70+row*y+x/4]>>(6-2*(x%4)))&3];
  unsigned populated=0;for(unsigned n:counts)populated+=n>0;assert(populated>=2);
  std::printf("Valid production 2-bit BMP %dx%d, %zu bytes, palette pixels %u/%u/%u/%u\n",w,h,b.size(),counts[0],counts[1],counts[2],counts[3]);
}
int main(int argc,char** argv){
  if(argc<2){std::fprintf(stderr,"usage: cover_pipeline INPUT.jpg [OUTPUT_PREFIX]\n");return 2;}
  FILE* f=std::fopen(argv[1],"rb");assert(f);HalFile jpeg;assert(Storage.openFileForWrite("test","/test.jpg",jpeg));
  unsigned char chunk[1024];size_t n;while((n=std::fread(chunk,1,sizeof(chunk),f)))assert(jpeg.write(chunk,n)==n);std::fclose(f);jpeg.close();assert(Storage.openFileForRead("test","/test.jpg",jpeg));
  ESP.setFreeHeap(109924);ESP.setMaxAllocHeap(63476);
  for(int mode=0;mode<2;++mode){
    assert(jpeg.seek(0));Output out;
    bool ok=mode?JpegToBmpConverter::jpegFileToBmpStream(jpeg,out,false):JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(jpeg,out,420,560);
    if(!ok){std::fprintf(stderr,"Production cover conversion FAILED mode=%d\n",mode);return 1;}
    validate(out,mode?528:420,mode?792:560);
    if(argc>2){const auto p=std::string(argv[2])+(mode?"-sleep.bmp":"-bare.bmp");FILE* w=std::fopen(p.c_str(),"wb");assert(w);assert(std::fwrite(out.bytes.data(),1,out.bytes.size(),w)==out.bytes.size());std::fclose(w);}
  }
  std::puts("PASS production converter + actual HAL + FAT-style seek contract; host, not device.");
}
