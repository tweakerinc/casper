// Reproduction uses the production coefficient decoder and production spill I/O
// helper, but a filesystem which deliberately rejects every seek past EOF.
// An optional local image argument is never copied into repository fixtures.
#include "jpgd.h"
#include "jpgd_spill.h"
#include "SpillFileIo.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <cstdint>
#include <string>

struct File {
  FILE* f=std::tmpfile();
  int writesBeforeFailure=-1, readsBeforeFailure=-1;
  uint64_t written=0;unsigned sparseRejected=0,reads=0,writes=0;
  ~File(){if(f)std::fclose(f);}
  explicit operator bool()const{return f;}
  uint64_t fileSize64(){const auto p=ftello(f);fseeko(f,0,SEEK_END);auto n=ftello(f);fseeko(f,p,SEEK_SET);return n;}
  bool seek64(uint64_t p){if(p>fileSize64()){++sparseRejected;return false;}return fseeko(f,p,SEEK_SET)==0;}
  int read(void* p,size_t n){++reads;if(readsBeforeFailure==0)return -1;if(readsBeforeFailure>0)--readsBeforeFailure;return fread(p,1,n,f);}
  size_t write(const void*p,size_t n){++writes;if(writesBeforeFailure==0)return 0;if(writesBeforeFailure>0)--writesBeforeFailure;size_t w=fwrite(p,1,n,f);written+=w;return w;}
};
int rd(void*c,uint64_t o,void*b,int n){return coverdecode::readSpill(*static_cast<File*>(c),o,b,n);}
int wr(void*c,uint64_t o,const void*b,int n){return coverdecode::writeSpill(*static_cast<File*>(c),o,b,n);}
int legacyWr(void*c,uint64_t o,const void*b,int n){auto&f=*static_cast<File*>(c);return f.seek64(o)?f.write(b,n):0;}
std::vector<uint8_t> decode(const char*path,File* backing,bool legacy=false){
  jpgd::jpeg_decoder_file_stream src;if(!src.open(path))return {};
  if(backing){jpgd::jpeg_decoder_spill_io io{backing,rd,legacy?legacyWr:wr};if(!jpgd::jpgd_spill_begin(&io))return {};}
  auto d=std::make_unique<jpgd::jpeg_decoder>(&src,jpgd::jpeg_decoder::cFlagDisableSIMD|jpgd::jpeg_decoder::cFlagCoverDecode);
  std::vector<uint8_t> out;
  if(d->get_error_code()==0&&d->begin_decoding()==0){
    const int w=d->get_width(),h=d->get_height();out.resize(size_t(w)*h);
    uint8_t block[64];bool ok=true;
    for(int y=0;ok&&y<d->luma_blocks_y();++y)for(int x=0;ok&&x<d->luma_blocks_x();++x){
      ok=d->copy_luma_block(x,y,block)!=0;
      if(ok)for(int r=0;r<8&&y*8+r<h;++r){int n=std::min(8,w-x*8);if(n>0)std::memcpy(out.data()+size_t(y*8+r)*w+x*8,block+r*8,n);}
    }
    if(!ok)out.clear();
  }
  if(legacy)printf("Legacy spill result: error=%d seek-past-EOF=%u\n",d->get_error_code(),backing->sparseRejected);
  d.reset();if(backing)jpgd::jpgd_spill_end();return out;
}
int main(int argc,char**argv){
  if(argc<2)return 2;
  const auto expected=decode(argv[1],nullptr);if(expected.empty()){puts("reference decode FAILED");return 1;}
  File before;const auto old=decode(argv[1],&before,true);if(old!=expected || before.sparseRejected){puts("compact store requested sparse I/O");return 1;}
  File after;const auto got=decode(argv[1],&after);
  if(got!=expected||after.sparseRejected){puts("fixed luma comparison FAILED");return 1;}
  if(argc>2){FILE*f=fopen(argv[2],"wb");fwrite(got.data(),1,got.size(),f);fclose(f);}
  // A real disk read/write failure must never turn into a valid blank cover.
  File failWrite;failWrite.writesBeforeFailure=2;if(!decode(argv[1],&failWrite).empty())return 1;
  File failRead;failRead.readsBeforeFailure=2;if(!decode(argv[1],&failRead).empty())return 1;
  File gap;const uint8_t mark[]={1,2,3,4};uint8_t check[4]{};
  if(coverdecode::writeSpill(gap,2048,mark,4)!=4||gap.fileSize64()!=2052)return 1;
  if(coverdecode::readSpill(gap,0,check,4)!=4||check[0]||check[1]||check[2]||check[3])return 1;
  if(coverdecode::readSpill(gap,2048,check,4)!=4||std::memcmp(check,mark,4))return 1;
  if(coverdecode::writeSpill(gap,UINT64_MAX,mark,4)!=-1)return 1;
  printf("PASS: %zu luma pixels exactly match resident reference; strict-SD reads=%u writes=%u bytes-written=%llu; faults rejected\n",got.size(),after.reads,after.writes,(unsigned long long)after.written);
}
