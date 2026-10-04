// Actual ChapterLoader + ReadinessCoordinator + engine + storage adapter.
// The lower filesystem/RTOS, font metrics and ZIP transport are test doubles.
#include <cassert>
#include <fstream>
#include <memory>
#include <vector>
#include <cstdio>
#include "ChapterLoader.h"
#include "ReadinessCoordinator.h"
#include "ChapterGeometry.h"
#include "Esp.h"
#include <GfxRenderer.h>
EspStub ESP;
namespace chaptergeometry {
size_t prepareRange(Epub&,GfxRenderer&,rivulet::RivuletEngine& e,const std::string&,uint8_t,size_t first,size_t n){return std::min(first+n,e.chapter().blockCount());}
void prepare(Epub&,GfxRenderer&,rivulet::RivuletEngine&,const std::string&,uint8_t){}
}
using namespace rivulet;
using RC=ReadinessCoordinator;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL navigation %d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Book {
 Epub epub;GfxRenderer renderer;std::string dir="/navigation";bool abort=false;unsigned steps=0;
 struct Task:RC::Preparation {
  Book& book;chapterload::Session session;
  Task(Book&b,RivuletEngine&e,int s):book(b),session(request(b,e,s),hooks(b)){}
  static chapterload::Request request(Book&b,RivuletEngine&e,int s){chapterload::Request r;r.epub=&b.epub;r.engine=&e;r.renderer=&b.renderer;r.irDir=b.dir;r.spineIndex=s;return r;}
  static chapterload::Hooks hooks(Book&b){chapterload::Hooks h;h.ctx=&b;h.shouldAbort=[](void*p){return static_cast<Book*>(p)->abort;};return h;}
  RC::Load step()override{++book.steps;const auto s=session.step();
   if(s==chapterload::Session::Status::Working)return RC::Load::Working;
   if(s==chapterload::Session::Status::Cancelled)return RC::Load::Cancelled;
   if(s==chapterload::Session::Status::Failed)return RC::Load::Failed;
   return session.result().empty?RC::Load::Empty:session.result().ok?RC::Load::Ready:RC::Load::Failed;
  }
 };
 static std::unique_ptr<RC::Preparation> factory(void*p,RivuletEngine&e,int s){return std::make_unique<Task>(*static_cast<Book*>(p),e,s);}
};
static RC::Tick drive(RC& rc,Book&book,int current,const RivuletEngine* active=nullptr){
 const auto pos=active?active->currentStartCursor():IrCursor{};
 const int page=active?active->currentPage():0;
 for(unsigned ticks=0;ticks<100000;++ticks){
  auto s=rc.tickPrepared(book.renderer,current,&Book::factory,&book,100000+ticks*100,nullptr);
  if(active){CHECK(active->currentStartCursor()==pos);CHECK(active->currentPage()==page);CHECK(active->hasPreparedPage());}
  if(s==RC::Tick::NavigationReady){CHECK(rc.readyEngine()!=nullptr);CHECK(rc.readyEngine()->hasPreparedPage());return s;}
  if(s==RC::Tick::NavigationFailed)return s;
 }
 std::fprintf(stderr,"stuck worker=%d target=%d reason=%s\n",rc.workerSpine(),rc.destination(),RC::failureName(rc.lastFailure()));CHECK(false);return RC::Tick::Idle;
}
static uint32_t digest(const LaidOutPage&p){
 uint32_t h=2166136261U;for(const auto&s:p.spans){for(size_t i=0;i<s.text.size();++i){h^=uint8_t(s.text[i]);h*=16777619U;}h^=uint32_t(uint16_t(s.x))<<16|uint16_t(s.y);h*=16777619U;}return h;
}
int main(int argc,char**argv){
 if(argc!=3)return 2;Book book;book.epub.root=argv[1];std::ifstream in(argv[2]);std::string s;while(std::getline(in,s))if(!s.empty())book.epub.spines.push_back(s);
 CHECK(book.epub.getSpineItemsCount()>0);
 RenderKey key;key.fontId=-1128177077;key.viewportW=502;key.viewportH=704;key.flags=1;
 RC rc;CHECK(rc.configure(book.dir,key,1,book.epub.getSpineItemsCount()));
 std::unique_ptr<RivuletEngine> active;unsigned pages=0;
 for(int spine=0;spine<book.epub.getSpineItemsCount();++spine){
  CHECK(rc.request(spine,0));auto state=drive(rc,book,spine,active.get());
  if(state==RC::Tick::NavigationFailed && rc.lastFailure()==RC::Failure::EmptyDestination)continue;
  if(state!=RC::Tick::NavigationReady)std::fprintf(stderr,"spine=%d first reason=%s\n",spine,RC::failureName(rc.lastFailure()));
  CHECK(state==RC::Tick::NavigationReady);active=rc.takeReady();CHECK(active->currentPage()==0);
  // Same conversion path must provide every page, not just a first-page smoke test.
  std::vector<IrCursor> starts;std::vector<uint32_t> hashes;
  for(unsigned walked=0;walked<10000;++walked){
   CHECK(active->hasPreparedPage());CHECK(!active->chapter().failed());
   starts.push_back(active->page().start);hashes.push_back(digest(active->page()));
   if(active->page().atChapterEnd)break;
   const auto end=active->page().end;CHECK(active->nextPage(book.renderer));CHECK(active->page().start==end);
  }
  CHECK(active->page().atChapterEnd);CHECK(active->sealMapAtChapterEnd());CHECK(active->mapComplete());
  CHECK(active->mapKnownPages()==int(starts.size()));
  const unsigned count=starts.size();pages+=count;
  // Walk all the way back through the real engine; no duplicated/skipped text.
  while(active->currentPage()>0){CHECK(active->prevPage(book.renderer));const auto p=active->currentPage();CHECK(active->page().start==starts[p]);CHECK(digest(active->page())==hashes[p]);}
  // A first-page-released background engine promoted to a last-page request
  // must give the exact verified ending while the active first page is intact.
  CHECK(rc.request(spine,-1));CHECK(drive(rc,book,spine,active.get())==RC::Tick::NavigationReady);
  auto last=rc.takeReady();CHECK(last->page().atChapterEnd);CHECK(last->currentPage()==int(count)-1);
  CHECK(last->page().start==starts.back());CHECK(digest(last->page())==hashes.back());
  ProgressAnchor anchor;CHECK(ProgressAnchor::capture(*last,123,spine,anchor));
  CHECK(ProgressAnchor::save(book.dir,anchor));ProgressAnchor disk;CHECK(ProgressAnchor::load(book.dir,123,disk));
  rc.release();CHECK(rc.configure(book.dir,key,1,book.epub.getSpineItemsCount()));CHECK(rc.requestAnchor(disk));
  CHECK(drive(rc,book,spine,active.get())==RC::Tick::NavigationReady);auto resumed=rc.takeReady();
  CHECK(resumed->currentStartCursor()==starts.back());CHECK(digest(resumed->page())==hashes.back());
  std::printf("navigation spine=%d all-pages=%u reverse/tail/reopen exact\n",spine,count);
 }
 // Transfer failure and cancellation leave the last successfully selected page.
 CHECK(active);book.dir="/failure";rc.release();CHECK(rc.configure(book.dir,key,1,book.epub.getSpineItemsCount()));
 book.epub.failTransfer=true;CHECK(rc.request(0,0));CHECK(drive(rc,book,0,active.get())==RC::Tick::NavigationFailed);CHECK(rc.lastFailure()==RC::Failure::AcquireChapter);book.epub.failTransfer=false;
 CHECK(rc.request(0,0));book.abort=true;for(unsigned i=0;i<10;++i)CHECK(rc.tickPrepared(book.renderer,0,&Book::factory,&book,i,nullptr)!=RC::Tick::NavigationReady);
 book.abort=false;CHECK(drive(rc,book,0,active.get())==RC::Tick::NavigationReady);rc.consumed();
 std::printf("PASS production navigation: %u pages, %u checks; hardware/fonts/ZIP transport mocked.\n",pages,checks);
}
