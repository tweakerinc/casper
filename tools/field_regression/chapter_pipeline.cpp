// Real ChapterLoader + parser + paged records + HAL. Transport, fonts and
// image geometry are mocked explicitly; this is not a panel/speed test.
#include <ChapterLoader.h>
#include <ChapterGeometry.h>
#include <GfxRenderer.h>
#include <PreparationBudget.h>
#include <cassert>
#include <fstream>
#include <cstdio>
EspStub ESP;
namespace chaptergeometry {
size_t prepareRange(Epub&,GfxRenderer&,rivulet::RivuletEngine& e,const std::string&,uint8_t,size_t first,size_t n){return std::min(first+n,e.chapter().blockCount());}
void prepare(Epub&,GfxRenderer&,rivulet::RivuletEngine&,const std::string&,uint8_t){}
}
static chapterload::Result load(Epub& book,rivulet::RivuletEngine& eng,GfxRenderer& renderer,int index,const std::string& dir){
 chapterload::Request req;req.epub=&book;req.engine=&eng;req.renderer=&renderer;req.irDir=dir;req.spineIndex=index;
 chapterload::Session session(req);for(int steps=0;steps<10000;++steps){auto s=session.step();if(s==chapterload::Session::Status::Done)return session.result();if(s!=chapterload::Session::Status::Working){std::fprintf(stderr,"loader failed spine=%d\n",index);return {};}}assert(false);return {};
}
static std::string text(const rivulet::ChapterIr& ir){std::string out;for(const auto run:ir.runs()){out.append(ir.runText(run),run.textLen);out+='\n';}return out;}
int main(int argc,char**argv){
 if(argc!=3){std::fprintf(stderr,"usage: chapter_pipeline EXTRACTED_ROOT SPINE_MANIFEST\n");return 2;}
 Epub book;book.root=argv[1];std::ifstream manifest(argv[2]);std::string line;while(std::getline(manifest,line))if(!line.empty())book.spines.push_back(line);
 assert(!book.spines.empty());GfxRenderer renderer;rivulet::RenderKey key;key.fontId=-1128177077;key.viewportW=502;key.viewportH=704;key.flags=1;
 size_t totalBlocks=0,totalRuns=0,totalText=0;
 for(int i=0;i<book.getSpineItemsCount();++i){
  rivulet::RivuletEngine e;e.setRenderKey(key);const std::string dir="/book";
  const auto cold=load(book,e,renderer,i,dir);assert(cold.ok||cold.empty);if(cold.empty)continue;
  const auto full=text(e.chapter());const auto blocks=e.chapter().blockCount(),runs=e.chapter().runs().size();
  assert(!e.chapter().failed());assert(e.goToStart(renderer));
  // Deferred publication must not write a page file before first paint.
  assert(!e.hasPageCache(i,0));assert(e.flushPageCache());assert(e.hasPageCache(i,0));
  rivulet::RivuletEngine warm;warm.setRenderKey(key);const auto transfers=book.transfers;
  const auto again=load(book,warm,renderer,i,dir);assert(again.ok&&again.fromCache);assert(book.transfers==transfers);
  assert(warm.chapter().blockCount()==blocks&&warm.chapter().runs().size()==runs);assert(text(warm.chapter())==full);
  assert(warm.goToStart(renderer));assert(warm.page().start==e.page().start&&warm.page().end==e.page().end);
  totalBlocks+=blocks;totalRuns+=runs;totalText+=e.chapter().textSize();
  std::printf("spine=%d blocks=%zu runs=%zu bytes=%zu cold/warm exact, first-page cache deferred\n",i,blocks,runs,e.chapter().textSize());
 }
 using namespace rivulet::preparationbudget;
 assert(!canStart(49336,26612));assert(!canStart(kStartFree-1,kStartLargest));assert(!canStart(kStartFree,kStartLargest-1));assert(canStart(kStartFree,kStartLargest));
 assert(!isQuiet(3000,1000,0));assert(isQuiet(4000,1000,0));assert(isQuiet(1000,uint32_t(1000-kQuietMs),uint32_t(1000-kQuietMs)));
 std::printf("PASS actual loader/IR/layout/cache/HAL: %d spines, %zu blocks, %zu runs, %zu text bytes; memory-admission policy checked.\n",book.getSpineItemsCount(),totalBlocks,totalRuns,totalText);
}
