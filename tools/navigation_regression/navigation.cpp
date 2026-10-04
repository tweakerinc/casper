#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include "Epub/TocSelectionPolicy.h"
#include "Epub/BookMetadataCache.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

unsigned checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<"\n"; std::abort(); } } while(0)
template<class T> bool parse(const std::string& xml, const std::string& base, epubnav::Targets& targets,
                             BookMetadataCache& output, size_t chunk) {
  T p(base,xml.size(),&output,&targets);
  CHECK(p.setup());
  for (size_t off=0; off<xml.size();) {
    size_t n=std::min(chunk,xml.size()-off);
    if(p.write(reinterpret_cast<const uint8_t*>(xml.data()+off),n)!=n) return false;
    off+=n;
  }
  return p.complete();
}
std::string read(const char* name) {
  std::ifstream f(name,std::ios::binary); CHECK(bool(f));
  return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int main(int argc,char**argv) {
  const std::string navXml="<html><body><nav type='toc'><ol><li><a href='../text/main.xhtml#part'>Part</a></li></ol></nav></body></html>";
  const std::string ncxXml="<ncx><navMap><navPoint><navLabel><text>Part</text></navLabel><content src='../text/main.xhtml#part'/><navPoint><navLabel><text>Chapter 1</text></navLabel><content src='../text/ch1.xhtml#one'/></navPoint><navPoint><navLabel><text>Chapter 2</text></navLabel><content src='../text/ch2.xhtml#two'/></navPoint></navPoint></navMap></ncx>";
  for (size_t chunk : {size_t(1),size_t(7),size_t(1024)}) {
    epubnav::Targets nav,ncx; BookMetadataCache n,c;
    CHECK(parse<TocNavParser>(navXml,"OPS/nav/",nav,n,chunk));
    CHECK(parse<TocNcxParser>(ncxXml,"OPS/nav/",ncx,c,chunk));
    CHECK(epubnav::preferNcx(nav,true,ncx,true));
    CHECK(n.entries.size()==1 && c.entries.size()==3);
    CHECK(c.entries[0].level==1 && c.entries[1].level==2 && c.entries[2].level==2);
    CHECK(c.entries[1].href=="OPS/text/ch1.xhtml" && c.entries[1].anchor=="one");
    CHECK(!epubnav::preferNcx(ncx,true,nav,true));
    CHECK(!epubnav::preferNcx(nav,true,ncx,false));
    CHECK(epubnav::preferNcx(nav,false,ncx,true));
    CHECK(!epubnav::useSpineFallback(1) && epubnav::useSpineFallback(0));
  }
  epubnav::Targets n,dup,conflict,wrongOrder,extra;
  n.add("a","x");n.add("b","y");
  dup.add("a","x");dup.add("a","x");dup.add("b","y");
  conflict.add("a","wrong");conflict.add("b","y");conflict.add("c","");
  wrongOrder.add("b","y");wrongOrder.add("a","x");wrongOrder.add("c","");
  extra.add("a","x");extra.add("a","second-fragment");extra.add("b","y");
  CHECK(!epubnav::preferNcx(n,true,dup,true));
  CHECK(!epubnav::preferNcx(n,true,conflict,true));
  CHECK(!epubnav::preferNcx(n,true,wrongOrder,true));
  CHECK(epubnav::preferNcx(n,true,extra,true));
  epubnav::Targets failed,empty;
  casper_memory::allocationsBeforeFailure=0;
  failed.add("a","x");
  casper_memory::allocationsBeforeFailure=-1;
  CHECK(!failed.valid());
  CHECK(!epubnav::preferNcx(failed,true,extra,true));
  CHECK(!epubnav::preferNcx(n,true,failed,true));
  CHECK(epubnav::preferNcx(empty,true,extra,true));
  CHECK(!epubnav::preferNcx(n,true,empty,true));
  {
    std::string base="OPS/";epubnav::Targets targets;BookMetadataCache out;
    TocNcxParser p(base,ncxXml.size()+20,&out,&targets); CHECK(p.setup());
    CHECK(p.write(reinterpret_cast<const uint8_t*>(ncxXml.data()),ncxXml.size())==ncxXml.size());
    CHECK(!p.complete());
  }
  if(argc==4) {
    std::string navXml=read(argv[1]),ncxXml=read(argv[2]),base=argv[3];
    for(size_t chunk : {size_t(1),size_t(17),size_t(1024)}) {
      epubnav::Targets nav,ncx;BookMetadataCache n,c;
      CHECK(parse<TocNavParser>(navXml,base,nav,n,chunk));
      CHECK(parse<TocNcxParser>(ncxXml,base,ncx,c,chunk));
      CHECK(epubnav::preferNcx(nav,true,ncx,true));
      CHECK(n.entries.size()==15 && c.entries.size()==125);
      int expected=1;
      for(const auto& e:c.entries) {
        CHECK(e.title.find("Section ")!=0);
        CHECK(!e.href.empty() && !e.anchor.empty());
        if(e.title.rfind("Chapter ",0)==0) {
          CHECK(e.title=="Chapter "+std::to_string(expected++));
          CHECK(e.level==2);
        }
      }
      CHECK(expected==99);
      std::cout<<"Private EPUB: NAV="<<n.entries.size()<<", NCX="<<c.entries.size()<<", numbered chapters="<<expected-1<<", preserved source labels/anchors/levels, chunk="<<chunk<<"\n";
    }
  }
  std::cout<<checks<<" navigation checks passed (actual XML parsers, host metadata sink).\n";
}
