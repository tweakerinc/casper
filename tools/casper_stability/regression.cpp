#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <Esp.h>
#include <HalStorage.h>
#include <GfxRenderer.h>
#include <FontDecompressor.h>
#include "ChapterIr.h"
#include "HtmlToIr.h"
#include "IrTokenizer.h"
#include "PageLayouter.h"
#include "RivuletEngine.h"
#include "FontCacheManager.h"
#include "FallibleString.h"
#include "BoundedUtf8.h"
#include "FontManifest.h"
EspStub ESP;
using namespace rivulet;
static int tests=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}++tests;}while(0)
static std::string flatten(const ChapterIr& ch){std::string s;for(const auto&r:ch.runs())s.append(ch.runText(r),r.textLen);return s;}
static std::vector<IrCursor> pages(const ChapterIr& ch, bool measure) {
  GfxRenderer renderer; LayoutParams p; p.key.fontId=-1128177077;p.key.viewportW=240;p.key.viewportH=320;p.key.flags=1;p.measureOnly=measure;
  IrCursor c{};if(ch.blocks().size())c.runIndex=ch.blocks()[0].runBegin;
  std::vector<IrCursor> out;
  for(int i=0;i<4000;++i) { LaidOutPage page;CHECK(PageLayouter::layoutPage(ch,renderer,p,c,page)); CHECK(!page.failed());out.push_back(c);if(page.atChapterEnd)return out;CHECK(c<page.end);c=page.end; }
  CHECK(false);return out;
}
int main(){
  using namespace casper_memory;
  { FallibleVector<uint32_t> v;CHECK(v.reserve(8));for(int i=0;i<8;++i)CHECK(v.push_back(i));allocationsBeforeFailure=0;CHECK(!v.push_back(8));CHECK(v.size()==8&&v[7]==7&&v.failed());allocationsBeforeFailure=-1;v.release();CHECK(v.capacity()==0); }
  { FallibleString s;CHECK(s.assign("small",5));allocationsBeforeFailure=0;CHECK(!s.assign(std::string(100,'x').c_str(),100));CHECK(s.view()=="small"&&s.failed());allocationsBeforeFailure=-1; }
  { const char raw[]={char(0xF0),char(0x9F)};const char*p=raw;CHECK(nextUtf8(p,raw+2)==0xFFFD);CHECK(p<=raw+2); }
  { ChapterIr ch;ch.beginBlock(BlockKind::Paragraph,Align::Left,0);std::string s(65534,'a');s+="\xe2\x80\x94";s+=std::string(6000,'b');CHECK(ch.appendRun(RunStyle::Regular,SizeStep::Body,s));ch.endBlock();CHECK(flatten(ch)==s);CHECK(ch.runs().size()==2);CHECK(ch.runs()[0].textLen==65534);CHECK(ch.saveToFile("/long.rvir"));ChapterIr r;CHECK(r.loadFromFile("/long.rvir"));CHECK(flatten(r)==s); }
  // Rich structural cases exercise the same source through paint and measure.
  std::vector<std::string> fixtures={
    "<h1 align='center'>Chapter 1</h1><p><span class='dropcap'>A</span>lice followed the rabbit.</p>",
    "<p>Before <b>bold</b> after <i>italic</i> and 6<sup>th</sup> place.</p>",
    "<h2>Heading</h2><p>One</p><p>Two</p><hr/><p>Three</p>",
    "<table><tr><td>Left cell</td><td>Right cell</td></tr></table>",
    "<p>日本語の文章。漢字（かな）混在！</p>"};
  for(const auto&html:fixtures){ChapterIr ch;CHECK(HtmlToIr::convert(html.data(),html.size(),ch));CHECK(pages(ch,false)==pages(ch,true));}
  { ChapterIr ch;ch.beginBlock(BlockKind::Paragraph,Align::Justify,0);std::string text;for(int i=0;i<12000;++i)text+="word ";CHECK(ch.appendRun(RunStyle::Regular,SizeStep::Body,text));ch.endBlock();auto full=pages(ch,false),measure=pages(ch,true);CHECK(full==measure);CHECK(full.size()>=90);std::printf("long-paragraph pages=%zu; paint/measure match\n",full.size());IrTokenCursor cur(ch,0,ch.runs().size(),0,0);IrTok t;size_t bytes=0;while(cur.next(t))bytes+=t.byteLen;CHECK(bytes==text.size()); }
  { RenderKey a,b;b.pad=0x20;CHECK(a!=b);PageMap m;m.setRenderKey(a);CHECK(m.resetWithStart({0,0,0}));CHECK(m.pushPageStart({0,0,5}));m.markComplete(2);CHECK(m.saveToFile("/map"));PageMap n;CHECK(n.loadFromFile("/map"));CHECK(n.complete()&&n.knownPages()==2);allocationsBeforeFailure=0;PageMap oom;CHECK(!oom.loadFromFile("/map"));CHECK(Storage.exists("/map"));allocationsBeforeFailure=-1; }
  { std::map<int,EpdFontFamily> fonts;fonts[-17]=EpdFontFamily{};fonts[22]=EpdFontFamily{};std::map<int,SdCardFont*> sd;FontDecompressor fd;FontCacheManager f(fonts,sd);f.setFontDecompressor(&fd);{auto scope=f.createPrewarmScope(false,false);std::string repeated(20000,'a');f.recordText(repeated.c_str(),-17,EpdFontFamily::REGULAR);f.recordText("b",22,EpdFontFamily::ITALIC);CHECK(!scope.endScanAndPrewarm());}CHECK(fd.calls.size()==2);CHECK(fd.calls[0].text=="a");CHECK(fd.calls[0].data==fonts[-17].getData()); }
  { RivuletEngine e;RenderKey k;k.fontId=-1128177077;k.viewportW=240;k.viewportH=320;k.flags=1;e.setRenderKey(k);std::string html="<p>";for(int i=0;i<3000;++i)html+="word ";html+="</p>";CHECK(e.ingestHtml(html.data(),html.size(),nullptr,false,0));GfxRenderer r;CHECK(e.goToStart(r));CHECK(e.extendPageMap(r,8));int known=e.mapKnownPages();CHECK(known>=8);CHECK(e.goToLastPage(r,4096,false));CHECK(e.mapComplete()&&e.page().atChapterEnd);CHECK(e.lastWalkPages()<e.mapKnownPages());int last=e.currentPage();CHECK(e.prevPage(r));CHECK(e.currentPage()==last-1);CHECK(e.nextPage(r));CHECK(e.currentPage()==last);e.chapterMutable().markFailed();CHECK(!e.goToLastPage(r,4096,true)); }
  {
    const std::string json=R"({"version":1,"baseUrl":"https://example.invalid/","families":[{"name":"Test","description":"Example","files":[{"name":"Test12.cpfont","size":1234,"crc32":4294967295}]}]})";
    for(size_t chunk=1;chunk<33;++chunk){fontmanifest::Parser p;for(size_t i=0;i<json.size();i+=chunk)p.feed(json.data()+i,std::min(chunk,json.size()-i));CHECK(p.ok());CHECK(p.families.size()==1);CHECK(p.families[0].files[0].crc32==UINT32_MAX);}
    fontmanifest::Parser truncated;truncated.feed(json.data(),json.size()-2);CHECK(!truncated.ok());
    allocationsBeforeFailure=0;fontmanifest::Parser oom;oom.feed(json.data(),json.size());CHECK(!oom.ok());allocationsBeforeFailure=-1;
  }
  std::printf("PASS %d assertions (real Rivulet source; mocked display/storage)\n",tests);
}
