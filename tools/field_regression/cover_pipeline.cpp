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
static int abortAfter = -1;
static int abortPolls = 0;
static bool coverAbort() { ++abortPolls; return abortAfter >= 0 && abortPolls >= abortAfter; }
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
  Output reference[2];
  for(int mode=0;mode<2;++mode){
    assert(jpeg.seek(0));Output out;
    bool ok=mode?JpegToBmpConverter::jpegFileToBmpStream(jpeg,out,false):JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(jpeg,out,420,560);
    if(!ok){std::fprintf(stderr,"Production cover conversion FAILED mode=%d\n",mode);return 1;}
    validate(out,mode?528:420,mode?792:560);
    reference[mode].bytes=out.bytes;
    if(argc>2){const auto p=std::string(argv[2])+(mode?"-sleep.bmp":"-bare.bmp");FILE* w=std::fopen(p.c_str(),"wb");assert(w);assert(std::fwrite(out.bytes.data(),1,out.bytes.size(),w)==out.bytes.size());std::fclose(w);}
  }
  assert(jpeg.seek(0)); Output thumb, sleep;
  abortAfter=-1;abortPolls=0;
  assert(JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(
      jpeg,thumb,420,560,coverAbort,&sleep,false));
  assert(thumb.bytes==reference[0].bytes && sleep.bytes==reference[1].bytes);
  const int completePolls=abortPolls;
  // Cancel before work, inside decode, and during late/secondary output; the
  // call must fail and release all scratch ownership. Retry must remain exact.
  for(int threshold : {1, std::max(2,completePolls/3), std::max(3,completePolls-5)}) {
    assert(jpeg.seek(0));Output cancelledThumb,cancelledSleep;
    abortAfter=threshold;abortPolls=0;
    assert(!JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(
        jpeg,cancelledThumb,420,560,coverAbort,&cancelledSleep,false));
    assert(abortPolls>=threshold);
  }
  assert(jpeg.seek(0));Output retryThumb,retrySleep;abortAfter=-1;abortPolls=0;
  assert(JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(
      jpeg,retryThumb,420,560,coverAbort,&retrySleep,false));
  assert(retryThumb.bytes==reference[0].bytes && retrySleep.bytes==reference[1].bytes);
  std::printf("PASS paired native-size cover output: exact reference bytes; cancellation at 3 stages, clean retry (%d polls)\n",completePolls);
  std::puts("PASS production converter + actual HAL + FAT-style seek contract; host, not device.");
}
