#include "HalStorage.h"
#include "SdFat.h"
#include "BookPageIndex.h"
#include "PagedRecords.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <utility>
int main(int argc,char**argv){
 if(argc>1 && std::strcmp(argv[1],"empty")==0){HalFile f;return f.close()?0:1;}
 if(argc>1 && std::strcmp(argv[1],"badread")==0){HalFile f;char b;(void)f.read(&b,1);return 0;}
 unsigned checks=0;
 auto require=[&](bool b){assert(b);++checks;};
 // Default, repeated cleanup and moved-from cleanup are deliberately harmless.
 HalFile empty; require(!empty.isOpen());require(empty.close());require(empty.close());
 HalFile a=Storage.open("/a",O_RDWR|O_CREAT|O_TRUNC);
 require(a.isOpen());require(a.write(uint8_t{42})==1);
 HalFile b(std::move(a)); require(!a.isOpen());require(a.close());require(b.isOpen());
 require(b.seek(0));require(b.read()==42);require(b.close());require(!b.isOpen());require(b.close());
 // A failed open creates a valid, closed implementation, not an open file.
 HalFile missing;require(!Storage.openFileForRead("T","/missing",missing));require(!missing.isOpen());require(missing.close());
 HalFile x=Storage.open("/x",O_RDWR|O_CREAT|O_TRUNC);
 HalFile y=Storage.open("/y",O_RDWR|O_CREAT|O_TRUNC);
 unsigned before=fsprobe::closes;y=std::move(x);require(fsprobe::closes==before+1);require(x.close());require(y.isOpen());
 // Genuine filesystem close errors must still be returned, never hidden.
 fsprobe::failClose=true;require(!y.close());require(y.isOpen());fsprobe::failClose=false;require(y.close());require(y.close());
 // Replacing an out-parameter closes the prior file under the recursive lock.
 HalFile out;require(Storage.openFileForWrite("T","/out",out));before=fsprobe::closes;
 require(Storage.openFileForWrite("T","/again",out));require(fsprobe::closes==before+1);require(out.close());
 // Exercise the new Rivulet owners against the actual HAL translation unit.
 // Default/moved-from store cleanup is the device behavior the old mock missed.
 {
  rivulet::BookPageIndex index;
  index.close();index.close();
  rivulet::RenderKey key{};key.viewportW=528;key.viewportH=792;
  require(index.open("/index",key,3));require(index.record(1,7));
  uint32_t pages=0;require(index.read(1,pages));require(pages==7);
  index.close();index.close();require(index.open("/index",key,3));
  require(index.read(1,pages));require(pages==7);
 }
 {
  rivulet::PagedRecords<uint32_t,4> source;
  source.release();require(source.create("/records"));
  for(uint32_t i=0;i<12;++i)require(source.push_back(i*7));
  rivulet::PagedRecords<uint32_t,4> moved(std::move(source));
  source.release();require(moved.size()==12);
  const auto& checked=moved;
  for(uint32_t i=0;i<12;++i)require(checked[i]==i*7);
  require(moved.flush());moved.release();moved.release();
 }
 require(storageTestLockDepth==0);
 std::cout<<checks<<" lifecycle checks passed using actual lib/hal/HalStorage.cpp and HalStorage.h\n";
}
