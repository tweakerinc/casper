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
#include "ReadinessCoordinator.h"
#include "SourceIdentity.h"
#include "ProgressAnchor.h"
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

struct TestBook {
  std::vector<int> loads;
  int failSpine=-1;
  bool cancel=false;
  static ReadinessCoordinator::Load load(void* ctx,RivuletEngine& e,int spine){
    auto& self=*static_cast<TestBook*>(ctx);self.loads.push_back(spine);
    if(self.cancel)return ReadinessCoordinator::Load::Cancelled;
    if(spine==self.failSpine)return ReadinessCoordinator::Load::Failed;
    char ir[80],map[80];std::snprintf(ir,sizeof(ir),"/book/s%d_m0.rvir",spine);std::snprintf(map,sizeof(map),"/book/s%d_m0.rvpm",spine);
    e.setPageCacheDir("/book/pages");e.setPageCacheSpine(spine);
    if(!e.loadIr(ir)){
      std::string html="<h1>Chapter "+std::to_string(spine+1)+"</h1><p>";
      for(int j=0;j<2400;++j)html+="rabbit ";
      html+="VERIFIED END "+std::to_string(spine+1)+"</p>";
      HalFile w;CHECK(Storage.openFileForWrite("TEST","/target.html",w));w.write(html.data(),html.size());w.close();
      HalFile f;CHECK(Storage.openFileForRead("TEST","/target.html",f));CHECK(e.ingestHtmlFile(f,ir,0));CHECK(e.chapter().saveToFile(ir));
    }
    if(!e.loadPageMap(map)){std::strcat(map,".part");(void)e.loadPageMap(map);}
    return ReadinessCoordinator::Load::Ready;
  }
};
struct PreparedTestBook {
  std::string dir="/prepared";
  int steps=0;
  struct Job:ReadinessCoordinator::Preparation {
    PreparedTestBook& book;RivuletEngine& engine;int spine;HalFile input;
    std::unique_ptr<HtmlToIrSession> parser;bool initialized=false;
    char path[96]{};
    Job(PreparedTestBook&b,RivuletEngine&e,int s):book(b),engine(e),spine(s){}
    ReadinessCoordinator::Load step()override{
      ++book.steps;
      if(!initialized){
        std::snprintf(path,sizeof(path),"%s/s%d_m0.rvir",book.dir.c_str(),spine);
        std::string name=book.dir+"/s"+std::to_string(spine)+".html";
        if(!Storage.openFileForRead("TEST",name,input))return ReadinessCoordinator::Load::Failed;
        parser.reset(new HtmlToIrSession(input,path,engine.chapterMutable(),false,0));
        initialized=true;return ReadinessCoordinator::Load::Working;
      }
      auto result=parser->step(1024);
      if(result==HtmlToIrSession::Result::Working)return ReadinessCoordinator::Load::Working;
      if(result==HtmlToIrSession::Result::Failed)return ReadinessCoordinator::Load::Failed;
      parser.reset();input.close();
      if(!engine.adoptIngestedChapter(path))return ReadinessCoordinator::Load::Failed;
      return engine.hasChapter()?ReadinessCoordinator::Load::Ready:ReadinessCoordinator::Load::Empty;
    }
  };
  static std::unique_ptr<ReadinessCoordinator::Preparation> factory(void*ctx,RivuletEngine&e,int s){
    return std::unique_ptr<ReadinessCoordinator::Preparation>(new Job(*static_cast<PreparedTestBook*>(ctx),e,s));
  }
};
int main(){
  using namespace casper_memory;
  { FallibleVector<uint32_t> v;CHECK(v.reserve(8));for(int i=0;i<8;++i)CHECK(v.push_back(i));allocationsBeforeFailure=0;CHECK(!v.push_back(8));CHECK(v.size()==8&&v[7]==7&&v.failed());allocationsBeforeFailure=-1;v.release();CHECK(v.capacity()==0); }
  { FallibleString s;CHECK(s.assign("small",5));allocationsBeforeFailure=0;CHECK(!s.assign(std::string(100,'x').c_str(),100));CHECK(s.view()=="small"&&s.failed());allocationsBeforeFailure=-1; }
  { const char raw[]={char(0xF0),char(0x9F)};const char*p=raw;CHECK(nextUtf8(p,raw+2)==0xFFFD);CHECK(p<=raw+2); }
  { ChapterIr ch;ch.beginBlock(BlockKind::Paragraph,Align::Left,0);std::string s(65534,'a');s+="\xe2\x80\x94";s+=std::string(6000,'b');CHECK(ch.appendRun(RunStyle::Regular,SizeStep::Body,s));ch.endBlock();CHECK(flatten(ch)==s);CHECK(ch.runs().size()>2);for(const auto&run:ch.runs()) CHECK(run.textLen<=ChapterIr::kMaxRunBytes);CHECK(ch.saveToFile("/long.rvir"));ChapterIr r;CHECK(r.loadFromFile("/long.rvir"));CHECK(flatten(r)==s); }
  // RC2 -> RC3 saved starts: a new heading boundary changes the hash length
  // without changing which chapter the reader requested.
  {
    ChapterIr ch;
    ch.beginBlock(BlockKind::Heading1, Align::Center, 0);
    CHECK(ch.appendRun(RunStyle::Bold, SizeStep::Plus1, "Interlude", 9));
    ch.endBlock();
    ch.beginBlock(BlockKind::Paragraph, Align::Left, 0);
    CHECK(ch.appendRun(RunStyle::Regular, SizeStep::Body, "A dragon followed the reader.", 29));
    ch.endBlock();
    ProgressAnchor old;
    old.source=0x1234;old.spine=8;old.page=0;old.format=28;
    old.textOffset=0;old.cursor={0,0,0};
    old.context=ProgressAnchor::hash("InterludeA drago",16);
    IrCursor recovered;
    CHECK(old.resolve(ch,recovered));CHECK(recovered==IrCursor{});
    CHECK(ProgressAnchor::save("/migration",old));
    ProgressAnchor disk;CHECK(ProgressAnchor::load("/migration",0x1234,disk));
    CHECK(disk.resolve(ch,recovered));
    CHECK(disk.format==28); // resolving does not overwrite user state
    old.format=kIrFormatVersion;CHECK(!old.resolve(ch,recovered));
    old.format=kIrFormatVersion+1;CHECK(!old.resolve(ch,recovered));
    old.format=28;old.page=4;old.textOffset=999999;CHECK(!old.resolve(ch,recovered));
    ch.markFailed();old.page=0;old.textOffset=0;CHECK(!old.resolve(ch,recovered));
  }
  {
    // Same text, more style runs after parser upgrade: old context spans the
    // new boundary but must still match in full, at the exact saved offset.
    ChapterIr ch;ch.beginBlock(BlockKind::Paragraph,Align::Left,0);
    CHECK(ch.appendRun(RunStyle::Regular,SizeStep::Body,"first ABC",9));
    CHECK(ch.appendRun(RunStyle::Bold,SizeStep::Body,"DEFGHIJKLMNOPQRSTUV",19));ch.endBlock();
    ProgressAnchor old;old.format=28;old.page=2;old.textOffset=6;old.cursor={0,0,6};
    old.context=ProgressAnchor::hash("ABCDEFGHIJKLMNOP",16);
    IrCursor found;CHECK(old.resolve(ch,found));CHECK(found.byteInRun==6);
    old.context^=1;CHECK(!old.resolve(ch,found));
  }
  // Rich structural cases exercise the same source through paint and measure.
  std::vector<std::string> fixtures={
    "<h1 align='center'>Chapter 1</h1><p><span class='dropcap'>A</span>lice followed the rabbit.</p>",
    "<p>Before <b>bold</b> after <i>italic</i> and 6<sup>th</sup> place.</p>",
    "<h2>Heading</h2><p>One</p><p>Two</p><hr/><p>Three</p>",
    "<table><tr><td>Left cell</td><td>Right cell</td></tr></table>",
    "<p>日本語の文章。漢字（かな）混在！</p>"};
  for(const auto&html:fixtures){ChapterIr ch;CHECK(HtmlToIr::convert(html.data(),html.size(),ch));CHECK(pages(ch,false)==pages(ch,true));}
  { ChapterIr ch;ch.beginBlock(BlockKind::Paragraph,Align::Justify,0);std::string text;for(int i=0;i<12000;++i)text+="word ";CHECK(ch.appendRun(RunStyle::Regular,SizeStep::Body,text));ch.endBlock();auto full=pages(ch,false),measure=pages(ch,true);CHECK(full==measure);CHECK(full.size()>=90);std::printf("long-paragraph pages=%zu; paint/measure match\n",full.size());IrTokenCursor cur(ch,0,ch.runs().size(),0,0);IrTok t;size_t bytes=0;while(cur.next(t))bytes+=t.byteLen;CHECK(bytes==text.size()); }
  { RenderKey a,b;b.pad=0x20;CHECK(a!=b);PageMap m;m.setRenderKey(a);CHECK(m.resetWithStart({0,0,0}));CHECK(m.pushPageStart({0,0,5}));m.markComplete(2);CHECK(m.saveToFile("/map"));PageMap n;CHECK(n.loadFromFile("/map"));CHECK(n.complete()&&n.knownPages()==2);allocationsBeforeFailure=0;PageMap oom;CHECK(oom.loadFromFile("/map"));CHECK(oom.diskBacked());CHECK(Storage.exists("/map"));allocationsBeforeFailure=-1; }
  { std::map<int,EpdFontFamily> fonts;fonts[-17]=EpdFontFamily{};fonts[22]=EpdFontFamily{};std::map<int,SdCardFont*> sd;FontDecompressor fd;FontCacheManager f(fonts,sd);f.setFontDecompressor(&fd);{auto scope=f.createPrewarmScope(false,false);std::string repeated(20000,'a');f.recordText(repeated.c_str(),-17,EpdFontFamily::REGULAR);f.recordText("b",22,EpdFontFamily::ITALIC);CHECK(!scope.endScanAndPrewarm());}CHECK(fd.calls.size()==2);CHECK(fd.calls[0].text=="a");CHECK(fd.calls[0].data==fonts[-17].getData()); }
  { RivuletEngine e;RenderKey k;k.fontId=-1128177077;k.viewportW=240;k.viewportH=320;k.flags=1;e.setRenderKey(k);std::string html="<p>";for(int i=0;i<3000;++i)html+="word ";html+="</p>";CHECK(e.ingestHtml(html.data(),html.size(),nullptr,false,0));GfxRenderer r;CHECK(e.goToStart(r));CHECK(e.extendPageMap(r,8));int known=e.mapKnownPages();CHECK(known>=8);CHECK(e.goToLastPage(r,4096,false));CHECK(e.mapComplete()&&e.page().atChapterEnd);CHECK(e.lastWalkPages()<e.mapKnownPages());int last=e.currentPage();CHECK(e.prevPage(r));CHECK(e.currentPage()==last-1);CHECK(e.nextPage(r));CHECK(e.currentPage()==last);e.chapterMutable().markFailed();CHECK(!e.goToLastPage(r,4096,true)); }
  {
    const std::string json=R"({"version":1,"baseUrl":"https://example.invalid/","families":[{"name":"Test","description":"Example","files":[{"name":"Test12.cpfont","size":1234,"crc32":4294967295}]}]})";
    for(size_t chunk=1;chunk<33;++chunk){fontmanifest::Parser p;for(size_t i=0;i<json.size();i+=chunk)p.feed(json.data()+i,std::min(chunk,json.size()-i));CHECK(p.ok());CHECK(p.families.size()==1);CHECK(p.families[0].files[0].crc32==UINT32_MAX);}
    fontmanifest::Parser truncated;truncated.feed(json.data(),json.size()-2);CHECK(!truncated.ok());
    allocationsBeforeFailure=0;fontmanifest::Parser oom;oom.feed(json.data(),json.size());CHECK(!oom.ok());allocationsBeforeFailure=-1;
  }

  // SD-backed full-stream conversion: no chapter-sized HTML/text/metadata buffer.
  {
    std::string html="<html><head><title>ignored</title></head><body><h1>Chapter 14</h1>";
    for(int i=0;i<5200;++i) html+="<p>Alice <b>followed</b> the rabbit &amp; the dragon \xe2\x80\x94 \xe6\x97\xa5\xe6\x9c\xac. More words to exercise a long chapter without resident metadata.</p>";
    html+="<p>THE VERIFIED END OF FOURTEEN</p></body></html>";
    HalFile w; CHECK(Storage.openFileForWrite("TEST","/large.html",w));CHECK(w.write(html.data(),html.size())==html.size());w.close();
    HalFile f;CHECK(Storage.openFileForRead("TEST","/large.html",f));ChapterIr disk;
    CHECK(HtmlToIr::convertFile(f,"/stream.rvir",disk,false,0));CHECK(disk.diskBacked());CHECK(!disk.failed());
    CHECK(disk.blockCount()==5202);CHECK(disk.textSize()>192*1024);
    const auto flat=flatten(disk);CHECK(flat.find("THE VERIFIED END OF FOURTEEN")!=std::string::npos);
    CHECK(flat.find("ignored")==std::string::npos);CHECK(flat.find("rabbit & the dragon")!=std::string::npos);
    CHECK(disk.saveToFile("/stream.rvir"));
    ChapterIr warm;CHECK(warm.loadFromFile("/stream.rvir"));CHECK(warm.diskBacked());CHECK(flatten(warm)==flat);
    CHECK(warm.setRunText(0,"replacement heading",19));CHECK(warm.saveToFile("/edited.rvir"));
    ChapterIr edit;CHECK(edit.loadFromFile("/edited.rvir"));CHECK(std::string(edit.runText(edit.runs()[0]))=="replacement heading");
  }
  {
    // Feed boundaries inside styles, entities, multi-byte text and a quoted '>'.
    std::string html="<body><h1 title='a > b'>Chapter</h1><p>";
    for(int i=0;i<1800;++i)html+="Alice <b>\xe6\x97\xa5\xe6\x9c\xac</b> &amp; rabbit \xe2\x80\x94 ";
    html+="</p></body>";
    ChapterIr mem,disk;CHECK(HtmlToIr::convert(html.data(),html.size(),mem));
    HalFile w;CHECK(Storage.openFileForWrite("TEST","/boundary.html",w));w.write(html.data(),html.size());w.close();
    HalFile r;CHECK(Storage.openFileForRead("TEST","/boundary.html",r));CHECK(HtmlToIr::convertFile(r,"/boundary.rvir",disk,false,0));
    CHECK(flatten(mem)==flatten(disk));CHECK(pages(mem,false)==pages(disk,false));CHECK(pages(disk,false)==pages(disk,true));
    int checks=0;auto cancel=[](void*ctx){return ++*static_cast<int*>(ctx)>4;};
    CHECK(r.seek(0));ChapterIr cancelled;CHECK(!HtmlToIr::convertFile(r,"/cancel.rvir",cancelled,false,0,cancel,&checks));CHECK(cancelled.failed());
    CHECK(!cancelled.saveToFile("/cancel.rvir"));
    allocationsBeforeFailure=0;ChapterIr oom;CHECK(r.seek(0));CHECK(!HtmlToIr::convertFile(r,"/oom.rvir",oom,false,0));allocationsBeforeFailure=-1;
  }

  {
    RenderKey key;key.fontId=-1128177077;key.viewportW=240;key.viewportH=320;key.flags=1;
    GfxRenderer renderer;TestBook book;RivuletEngine active;active.setRenderKey(key);
    CHECK(TestBook::load(&book,active,0)==ReadinessCoordinator::Load::Ready);CHECK(active.goToStart(renderer));
    ReadinessCoordinator jobs;CHECK(jobs.configure("/book",key,1,20));jobs.focus(0);
    // Chapter 1 -> 15 is transactional: even during source loading and page
    // layout, the active content remains chapter 1 until we publish the result.
    jobs.request(14,0);int ticks=0;auto status=ReadinessCoordinator::Tick::Idle;
    while(ticks++<10){status=jobs.tick(renderer,0,&TestBook::load,&book,ticks*100,nullptr);
      CHECK(flatten(active.chapter()).find("Chapter 1")!=std::string::npos);
      if(status==ReadinessCoordinator::Tick::NavigationReady)break;}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);auto ready=jobs.takeReady();CHECK(bool(ready));active=std::move(*ready);ready.reset();
    CHECK(flatten(active.chapter()).find("Chapter 15")!=std::string::npos);CHECK(active.currentPage()==0);jobs.focus(14);
    // Back into completely unindexed 14: one page per tick, never guessed tail.
    jobs.request(13,-1);ticks=0;int preceding=-1;
    while(ticks++<150){status=jobs.tick(renderer,14,&TestBook::load,&book,1000+ticks*100,nullptr);
      CHECK(active.currentPage()==0);CHECK(flatten(active.chapter()).find("Chapter 15")!=std::string::npos);
      if(status==ReadinessCoordinator::Tick::NavigationReady)break;
      CHECK(status!=ReadinessCoordinator::Tick::NavigationFailed);}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);CHECK(ticks>10);
    ready=jobs.takeReady();CHECK(ready->mapComplete());CHECK(ready->page().atChapterEnd);
    std::string text;for(const auto&sp:ready->page().spans){text.append(sp.text.data(),sp.text.size());text+=' ';}
    CHECK(text.find("END 14")!=std::string::npos);active=std::move(*ready);ready.reset();
    const int savedPage=active.currentPage();const auto savedStart=active.currentStartCursor();jobs.focus(13);
    // Forced load failure leaves the exact active page/cursor intact.
    book.failSpine=18;jobs.request(18,0);CHECK(jobs.tick(renderer,13,&TestBook::load,&book,50000,nullptr)==ReadinessCoordinator::Tick::NavigationFailed);
    CHECK(active.currentPage()==savedPage&&active.currentStartCursor()==savedStart);CHECK(!jobs.pending());book.failSpine=-1;
    // Interrupted indexing resumes from the saved .part, not chapter start.
    jobs.request(12,-1);for(int i=0;i<13;++i)jobs.tick(renderer,13,&TestBook::load,&book,60000+i*100,nullptr);
    jobs.checkpoint();jobs.release();CHECK(jobs.configure("/book",key,1,20));jobs.request(12,-1);
    ticks=0;while(ticks++<150){status=jobs.tick(renderer,13,&TestBook::load,&book,65000+ticks*100,nullptr);if(status==ReadinessCoordinator::Tick::NavigationReady)break;}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);CHECK(ticks<45);ready=jobs.takeReady();CHECK(ready->page().atChapterEnd);
    // Allocation failure is a recoverable request failure, not an endless spinner.
    jobs.request(19,0);allocationsBeforeFailure=0;CHECK(jobs.tick(renderer,13,&TestBook::load,&book,100000,nullptr)==ReadinessCoordinator::Tick::NavigationFailed);allocationsBeforeFailure=-1;
    CHECK(active.currentPage()==savedPage&&active.currentStartCursor()==savedStart);
    // Background starts behind the reader even when the current map is partial.
    ReadinessCoordinator around;CHECK(around.configure("/other",key,1,20));around.focus(14);book.loads.clear();
    CHECK(around.tick(renderer,14,&TestBook::load,&book,100000,nullptr)==ReadinessCoordinator::Tick::Working);CHECK(book.loads.back()==13);
  }
  {
    RenderKey key;BookPageIndex counts;CHECK(counts.open("/counts",key,16));CHECK(!counts.complete());
    for(uint32_t i=0;i<16;++i)CHECK(counts.record(i,i+1));CHECK(counts.complete()&&counts.total()==136);
    CHECK(counts.focus(14));CHECK(counts.prefixExact()&&counts.prefix()==105);
    counts.close();CHECK(counts.open("/counts",key,16));CHECK(counts.complete()&&counts.total()==136);
    auto other=key;other.pad=0x20;CHECK(counts.open("/counts",other,16));CHECK(!counts.complete());
    CHECK(counts.open("/counts",key,16));CHECK(counts.complete());CHECK(counts.forget(3));CHECK(!counts.complete());
    CHECK(counts.focus(14));CHECK(!counts.prefixExact());
  }

  {
    // Parser scheduling boundaries must not change formatting or drop text.
    std::string html="<html><head>"+std::string(19000,'h')+"</head><body><h1 title='a > b'>Opening</h1>";
    for(int i=0;i<250;++i)html+="<p>Alice <b>followed <i>the</i></b> rabbit &amp; dragon "+std::string("\xe6\x97\xa5\xe6\x9c\xac")+".</p>";
    html+="<p>END OF CHAPTER</p></body></html>";
    HalFile f;CHECK(Storage.openFileForWrite("TEST","/yield.html",f));CHECK(f.write(html.data(),html.size())==html.size());f.close();
    ChapterIr reference;CHECK(HtmlToIr::convert(html.data(),html.size(),reference));
    for(size_t budget:{size_t(128),size_t(1024),size_t(4096)}){
      CHECK(Storage.openFileForRead("TEST","/yield.html",f));ChapterIr yielded;
      HtmlToIrSession session(f,"/yield.rvir",yielded);
      int steps=0;size_t consumed=0;HtmlToIrSession::Result result;
      do{result=session.step(budget);CHECK(session.consumed()>=consumed);consumed=session.consumed();++steps;CHECK(steps<1500);}while(result==HtmlToIrSession::Result::Working);
      CHECK(result==HtmlToIrSession::Result::Done);CHECK(steps>5);CHECK(flatten(yielded)==flatten(reference));
      CHECK(pages(yielded,false)==pages(reference,false));
      // Canonical IR publication can pause at arbitrary byte boundaries.
      HalFile normal,chunked;CHECK(yielded.saveToFile("/normal.rvir"));
      CHECK(Storage.openFileForWrite("TEST","/chunked.rvir",chunked));
      for(size_t offset=0;offset<yielded.serializedSize();){const size_t n=std::min<size_t>(137,yielded.serializedSize()-offset);CHECK(yielded.writeRangeTo(chunked,offset,n));offset+=n;}
      chunked.close();CHECK(Storage.openFileForRead("TEST","/normal.rvir",normal));CHECK(Storage.openFileForRead("TEST","/chunked.rvir",chunked));CHECK(normal.size()==chunked.size());
      std::vector<char>a(normal.size()),b(chunked.size());CHECK(normal.read(a.data(),a.size())==int(a.size()));CHECK(chunked.read(b.data(),b.size())==int(b.size()));CHECK(a==b);
      ChapterIr loaded;CHECK(loaded.loadFromFile("/chunked.rvir"));CHECK(flatten(loaded)==flatten(reference));
    }
    // Read failure cannot masquerade as completed source.
    CHECK(Storage.openFileForRead("TEST","/yield.html",f));ChapterIr broken;HtmlToIrSession interrupted(f,"/broken.rvir",broken);
    storagefault::reads=0;auto failed=interrupted.step();for(int tries=0;tries<32 && failed==HtmlToIrSession::Result::Working;++tries)failed=interrupted.step();CHECK(failed==HtmlToIrSession::Result::Failed);storagefault::reset();CHECK(broken.failed());CHECK(!broken.saveToFile("/broken.rvir"));
  }
  {
    RenderKey key;key.fontId=-1128177077;key.viewportW=240;key.viewportH=320;key.flags=1;
    GfxRenderer renderer;PreparedTestBook book;
    for(int spine:{13,14}){
      std::string html="<h1>Chapter "+std::to_string(spine+1)+"</h1><p>";
      for(int j=0;j<1800;++j)html+="word ";html+="REAL END "+std::to_string(spine+1)+"</p>";
      HalFile f;CHECK(Storage.openFileForWrite("TEST",book.dir+"/s"+std::to_string(spine)+".html",f));CHECK(f.write(html.data(),html.size())==html.size());
    }
    ReadinessCoordinator jobs;CHECK(jobs.configure(book.dir,key,1,20));jobs.request(14,0);
    auto status=ReadinessCoordinator::Tick::Idle;int ticks=0;
    while(++ticks<300){status=jobs.tickPrepared(renderer,0,&PreparedTestBook::factory,&book,ticks*10,nullptr);if(status==ReadinessCoordinator::Tick::NavigationReady)break;CHECK(status!=ReadinessCoordinator::Tick::NavigationFailed);}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);CHECK(book.steps>8);
    auto active=jobs.takeReady();CHECK(bool(active));CHECK(active->goToPage(renderer,5,200));
    ProgressAnchor anchor;CHECK(ProgressAnchor::capture(*active,0x1234567812345678ULL,14,anchor));
    CHECK(ProgressAnchor::save("/anchors",anchor));ProgressAnchor recovered;CHECK(ProgressAnchor::load("/anchors",anchor.source,recovered));CHECK(recovered.cursor==anchor.cursor);
    CHECK(active->nextPage(renderer));ProgressAnchor later;CHECK(ProgressAnchor::capture(*active,anchor.source,14,later));
    storagefault::writes=0;CHECK(!ProgressAnchor::save("/anchors",later));storagefault::reset();
    CHECK(ProgressAnchor::load("/anchors",anchor.source,recovered));CHECK(recovered.cursor==anchor.cursor);
    CHECK(ProgressAnchor::save("/anchors",later));CHECK(ProgressAnchor::load("/anchors",anchor.source,recovered));CHECK(recovered.cursor==later.cursor);
    CHECK(!ProgressAnchor::load("/anchors",999,recovered));
    // Reflow from the same content anchor, not an old page fraction.
    auto newKey=key;newKey.viewportH=200;jobs.configure(book.dir,newKey,1,20);jobs.requestAnchor(anchor);
    ticks=0;while(++ticks<300){status=jobs.tickPrepared(renderer,14,&PreparedTestBook::factory,&book,ticks*10,nullptr);if(status==ReadinessCoordinator::Tick::NavigationReady)break;CHECK(status!=ReadinessCoordinator::Tick::NavigationFailed);}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);auto reflow=jobs.takeReady();
    CHECK(!(anchor.cursor<reflow->page().start));CHECK(anchor.cursor<reflow->page().end);CHECK(active->currentPage()==6);
    // A saved RC2 chapter-start hash does not survive RC3 block boundaries.
    // Exercise the actual coordinator, not just the anchor helper. The active
    // page remains untouched until the destination is ready to commit.
    ProgressAnchor legacyStart=anchor;
    legacyStart.format=28;legacyStart.page=0;legacyStart.textOffset=0;
    legacyStart.cursor={0,0,0};legacyStart.context=ProgressAnchor::hash("old joined title",16);
    const auto heldStart=active->currentStartCursor();
    jobs.requestAnchor(legacyStart);ticks=0;
    while(++ticks<300){
      status=jobs.tickPrepared(renderer,14,&PreparedTestBook::factory,&book,ticks*10,nullptr);
      CHECK(active->currentStartCursor()==heldStart);
      if(status==ReadinessCoordinator::Tick::NavigationReady)break;
      CHECK(status!=ReadinessCoordinator::Tick::NavigationFailed);
    }
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);
    auto migrated=jobs.takeReady();CHECK(bool(migrated));CHECK(migrated->currentPage()==0);
    // Exact percent navigation completes its own map while the active page stays intact.
    jobs.requestFraction(13,7500);ticks=0;const auto keep=active->currentStartCursor();
    while(++ticks<400){status=jobs.tickPrepared(renderer,14,&PreparedTestBook::factory,&book,ticks*10,nullptr);CHECK(active->currentStartCursor()==keep);if(status==ReadinessCoordinator::Tick::NavigationReady)break;CHECK(status!=ReadinessCoordinator::Tick::NavigationFailed);}
    CHECK(status==ReadinessCoordinator::Tick::NavigationReady);auto fraction=jobs.takeReady();CHECK(fraction->mapComplete());CHECK(fraction->currentPage()==fraction->mapKnownPages()*3/4);
    // A cancelled partially parsed worker is disposable, not a complete cache.
    jobs.request(14,-1);for(int i=0;i<5;++i)jobs.tickPrepared(renderer,13,&PreparedTestBook::factory,&book,i*10,nullptr);
    jobs.release();CHECK(active->currentStartCursor()==keep);
    // Transactional page navigation under an actual allocation failure.
    allocationsBeforeFailure=0;CHECK(!active->goToPage(renderer,999,1));allocationsBeforeFailure=-1;
    CHECK(active->currentStartCursor()==keep);
  }
  {
    // ZIP central directory identity includes entry CRCs, even when a replaced
    // book has the same file name and byte length. EOCD may cross a 1KiB boundary.
    auto makeZip=[](uint32_t crc,size_t comment){std::string z(91,'\0');
      auto put16=[&](size_t p,uint16_t v){z[p]=char(v);z[p+1]=char(v>>8);};
      auto put32=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)z[p+i]=char(v>>(8*i));};
      z.replace(32,4,"PK\001\002",4);put32(32+16,crc);put16(32+28,1);z[78]='x';
      z.resize(79+22+comment,'c');z.replace(79,4,"PK\005\006",4);for(int i=83;i<101;++i)z[i]=0;
      put16(79+8,1);put16(79+10,1);put32(79+12,47);put32(79+16,32);put16(79+20,comment);return z;
    };
    uint64_t previous=0;
    for(size_t comment:{size_t(0),size_t(1005),size_t(65535)}){
      const auto zip=makeZip(12345,comment);HalFile f;CHECK(Storage.openFileForWrite("TEST","/source.epub",f));CHECK(f.write(zip.data(),zip.size())==zip.size());f.close();
      uint64_t id=0;CHECK(sourceFingerprint("/source.epub",id));CHECK(id!=0);previous=id;
      const auto changed=makeZip(12346,comment);CHECK(changed.size()==zip.size());CHECK(Storage.openFileForWrite("TEST","/source.epub",f));CHECK(f.write(changed.data(),changed.size())==changed.size());f.close();
      CHECK(sourceFingerprint("/source.epub",id));CHECK(id!=previous);
    }
    storagefault::reads=0;CHECK(!sourceFingerprint("/source.epub",previous));storagefault::reset();
  }

  {
    // RC4: prefetch deliberately releases its paint page. Reusing that worker
    // for an explicit first-page request must not publish a failed layout.
    RenderKey key; key.fontId=-1128177077; key.viewportW=240; key.viewportH=320; key.flags=1;
    GfxRenderer renderer; TestBook book; ReadinessCoordinator jobs;
    CHECK(jobs.configure("/promotion-test", key, 1, 20)); jobs.focus(14);
    CHECK(jobs.tick(renderer,14,&TestBook::load,&book,100000,nullptr)==ReadinessCoordinator::Tick::Working);
    CHECK(jobs.workerSpine()==13);
    CHECK(jobs.tick(renderer,14,&TestBook::load,&book,100100,nullptr)==ReadinessCoordinator::Tick::Working);
    jobs.request(13,0);
    allocationsBeforeFailure=0;
    const auto status=jobs.tick(renderer,14,&TestBook::load,&book,100200,nullptr);
    allocationsBeforeFailure=-1;
    std::fprintf(stderr,"promotion status=%d readyEngine=%d spans=%zu\n",int(status), jobs.readyEngine()!=nullptr, jobs.readyEngine()?jobs.readyEngine()->page().spans.size():0);
    CHECK(status==ReadinessCoordinator::Tick::NavigationFailed);
    CHECK(jobs.readyEngine()==nullptr);
    CHECK(!jobs.pending());
  }

  {
    RenderKey key;key.fontId=-1128177077;key.viewportW=240;key.viewportH=320;key.flags=1;
    GfxRenderer renderer;TestBook book;ReadinessCoordinator jobs;
    CHECK(jobs.configure("/promotion-ok",key,1,20));jobs.focus(14);
    CHECK(jobs.tick(renderer,14,&TestBook::load,&book,100000,nullptr)==ReadinessCoordinator::Tick::Working);
    CHECK(jobs.tick(renderer,14,&TestBook::load,&book,100100,nullptr)==ReadinessCoordinator::Tick::Working);
    CHECK(jobs.request(13,0));
    CHECK(jobs.tick(renderer,14,&TestBook::load,&book,100200,nullptr)==ReadinessCoordinator::Tick::NavigationReady);
    CHECK(jobs.readyEngine()->hasPreparedPage());CHECK(!jobs.readyEngine()->page().spans.empty());
    // The ownership API cannot return a paint page that was released afterwards.
    jobs.readyEngine()->releasePaintPage();CHECK(jobs.readyEngine()==nullptr);CHECK(!jobs.takeReady());jobs.release();
    CHECK(jobs.configure("/requests",key,1,20));CHECK(jobs.request(3));
    ProgressAnchor bad;bad.spine=1000;CHECK(!jobs.requestAnchor(bad));CHECK(!jobs.pending());
    CHECK(jobs.lastFailure()==ReadinessCoordinator::Failure::InvalidTarget);
    CHECK(!jobs.request(0,-2));CHECK(!jobs.pending());
    CHECK(jobs.request(0,100000));
    auto state=ReadinessCoordinator::Tick::Idle;
    for(int i=0;i<300&&state!=ReadinessCoordinator::Tick::NavigationFailed;++i)
      state=jobs.tick(renderer,1,&TestBook::load,&book,200000+i*100,nullptr);
    CHECK(state==ReadinessCoordinator::Tick::NavigationFailed);
    CHECK(jobs.lastFailure()==ReadinessCoordinator::Failure::PageOutOfRange);
    CHECK(!jobs.pending());CHECK(!jobs.readyEngine());
    auto empty=[](void*,RivuletEngine&,int){return ReadinessCoordinator::Load::Empty;};
    CHECK(jobs.request(7,0));CHECK(jobs.tick(renderer,6,empty,nullptr,300000,nullptr)==ReadinessCoordinator::Tick::NavigationFailed);
    CHECK(jobs.lastFailure()==ReadinessCoordinator::Failure::EmptyDestination);
    CHECK(jobs.request(7,0,true));CHECK(jobs.tick(renderer,6,empty,nullptr,300100,nullptr)==ReadinessCoordinator::Tick::Working);
    CHECK(jobs.destination()==8);jobs.release();
  }
  {
    // Sweep actual allocation sites in background-to-foreground promotion.
    // Either the exact requested paint exists or navigation fails cleanly.
    for(int failAt=0;failAt<48;++failAt){
      RenderKey key;key.fontId=-1128177077;key.viewportW=240;key.viewportH=320;key.flags=1;
      GfxRenderer renderer;TestBook book;ReadinessCoordinator jobs;
      CHECK(jobs.configure("/promotion-sweep-"+std::to_string(failAt),key,1,20));jobs.focus(14);
      CHECK(jobs.tick(renderer,14,&TestBook::load,&book,1,nullptr)==ReadinessCoordinator::Tick::Working);
      CHECK(jobs.tick(renderer,14,&TestBook::load,&book,2,nullptr)==ReadinessCoordinator::Tick::Working);
      CHECK(jobs.request(13));allocationsBeforeFailure=failAt;
      const auto status=jobs.tick(renderer,14,&TestBook::load,&book,3,nullptr);allocationsBeforeFailure=-1;
      if(status==ReadinessCoordinator::Tick::NavigationReady){
        CHECK(jobs.readyEngine());CHECK(jobs.readyEngine()->hasPreparedPage());
        CHECK(jobs.readyEngine()->currentPage()==0);CHECK(!jobs.readyEngine()->page().spans.empty());
      }else{CHECK(status==ReadinessCoordinator::Tick::NavigationFailed);CHECK(!jobs.readyEngine());CHECK(!jobs.pending());}
    }
  }
  std::printf("PASS %d assertions (real Rivulet source; mocked display/storage)\n",tests);
}
