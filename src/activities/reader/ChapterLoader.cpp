#include "ChapterLoader.h"
#include "ChapterGeometry.h"
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HtmlToIr.h>
#include <Logging.h>
#include <Memory.h>
#include <algorithm>
#include <cstdio>
#include "util/TaskWatchdog.h"

namespace chapterload {
struct Session::Impl {
  Request req;
  Hooks hooks;
  Result result;
  Status status=Status::Working;
  enum Phase {Init,Extract,BeginParse,Parse,BeginPublish,Publish,Geometry,Map,Finish} phase=Init;
  HalFile input,output;
  std::unique_ptr<rivulet::HtmlToIrSession> parser;
  std::string href;
  char ir[224]{},html[224]{},temp[240]{},map[224]{},irTemp[240]{};
  size_t writeOffset=0,geometryOffset=0;
  uint32_t started=0;
  bool publishing=false;
  Impl(const Request& r,const Hooks& h):req(r),hooks(h){}
  ~Impl(){parser.reset();input.close();output.close();if(publishing)Storage.remove(irTemp);}
  bool abort() {resetTaskWatchdogIfSubscribed();return hooks.shouldAbort&&hooks.shouldAbort(hooks.ctx);}
  Status fail(){LOG_ERR("CHLOAD","failed spine=%d phase=%d free=%u maxA=%u",req.spineIndex,int(phase),unsigned(ESP.getFreeHeap()),unsigned(ESP.getMaxAllocHeap()));status=Status::Failed;parser.reset();input.close();output.close();return status;}
  Status step(){
    if(status!=Status::Working)return status;
    resetTaskWatchdogIfSubscribed();
    // A button is a scheduling interruption, not an invalid chapter. During
    // parsing retain the session and its style stack until input is handled.
    if(abort())return Status::Working;
    if(!req.epub||!req.engine||!req.renderer||req.irDir.empty())return fail();
    auto& eng=*req.engine;auto& epub=*req.epub;
    switch(phase){
      case Init: {
        started=millis();
        if(req.spineIndex<0||req.spineIndex>=epub.getSpineItemsCount())return fail();
        href=epub.getSpineItem(req.spineIndex).href;
        if(href.empty()){result.empty=true;status=Status::Done;return status;}
        if(!Storage.ensureDirectoryExists(req.irDir.c_str()))return fail();
        if(std::snprintf(ir,sizeof(ir),"%s/s%d_m%u.rvir",req.irDir.c_str(),req.spineIndex,req.imageRendering)>=int(sizeof(ir))||
           std::snprintf(html,sizeof(html),"%s/s%d.html",req.irDir.c_str(),req.spineIndex)>=int(sizeof(html))||
           std::snprintf(map,sizeof(map),"%s/s%d_m%u.rvpm",req.irDir.c_str(),req.spineIndex,req.imageRendering)>=int(sizeof(map)))return fail();
        std::snprintf(temp,sizeof(temp),"%s.tmp",html);std::snprintf(irTemp,sizeof(irTemp),"%s.tmp",ir);
        if(hooks.prepareHeap)hooks.prepareHeap(hooks.ctx,false);
        eng.clear();
        if(req.bindPageCache){eng.setPageCacheDir((req.irDir+"/pages").c_str());eng.setPageCacheSpine(req.spineIndex);}
        else{eng.clearPageCacheDir();eng.setPageCacheSpine(-1);}
        result.fromCache=eng.loadIr(ir);
        if(result.fromCache)phase=Geometry;
        else if(eng.lastIrLoadResult()==rivulet::RivuletEngine::IrLoadResult::Oom)return fail();
        else phase=Extract;
        return Status::Working;
      }
      case Extract: {
        if(!Storage.exists(html)){
          HalFile f;if(!Storage.openFileForWrite("CHLOAD",temp,f))return fail();
          // The inflater is still entry-scoped. Stop at its next output chunk
          // if a control is pressed; never borrow the displayed framebuffer.
          struct Sink:Print {HalFile& f;Impl& owner;bool cancelled=false;
            Sink(HalFile&f,Impl&o):f(f),owner(o){}
            size_t write(uint8_t b)override{return write(&b,1);}
            size_t write(const uint8_t*d,size_t n)override {if(owner.abort()){cancelled=true;return 0;}return f.write(d,n);}
          } sink(f,*this);
          bool ok=false;
          {GfxRenderer::FrameBufferLoan loan(*req.renderer,req.lendFrameBuffer);ok=epub.readItemContentsToStream(href,sink,4096);}
          const size_t size=f.size();f.flush();f.close();
          if(sink.cancelled){Storage.remove(temp);return Status::Working;}
          if(!ok){Storage.remove(temp);return fail();}
          if(!size){Storage.remove(temp);result.empty=true;status=Status::Done;return status;}
          if(!Storage.rename(temp,html)){Storage.remove(temp);return fail();}
        }
        phase=BeginParse;return Status::Working;
      }
      case BeginParse:
        if(!Storage.openFileForRead("CHLOAD",html,input))return fail();
        if(!input.size()){result.empty=true;status=Status::Done;return status;}
        parser=makeUniqueNoThrow<rivulet::HtmlToIrSession>(input,ir,eng.chapterMutable(),false,req.imageRendering);
        if(!parser)return fail();phase=Parse;return Status::Working;
      case Parse: {
        const auto parsed=parser->step(4096);
        if(parsed==rivulet::HtmlToIrSession::Result::Failed)return fail();
        if(parsed==rivulet::HtmlToIrSession::Result::Working)return Status::Working;
        parser.reset();input.close();
        if(!eng.adoptIngestedChapter(ir))return fail();
        if(eng.chapter().empty()){result.empty=true;status=Status::Done;return status;}
        phase=BeginPublish;return Status::Working;
      }
      case BeginPublish:
        // Canonical IR stores source geometry, never fitted image dimensions.
        if(Storage.openFileForWrite("CHLOAD",irTemp,output)){publishing=true;writeOffset=0;phase=Publish;}
        else phase=Geometry; // a derived-cache write failure need not hide valid text
        return Status::Working;
      case Publish: {
        const size_t total=eng.chapter().serializedSize();
        const size_t n=std::min<size_t>(4096,total-writeOffset);
        if(!eng.chapter().writeRangeTo(output,writeOffset,n)){
          output.close();Storage.remove(irTemp);publishing=false;
          if(eng.chapter().failed())return fail();phase=Geometry;return Status::Working;
        }
        writeOffset+=n;if(writeOffset<total)return Status::Working;
        output.flush();output.close();Storage.remove(ir);
        if(!Storage.rename(irTemp,ir))Storage.remove(irTemp);publishing=false;
        Storage.remove(map);char part[240];std::snprintf(part,sizeof(part),"%s.part",map);Storage.remove(part);
        phase=Geometry;return Status::Working;
      }
      case Geometry:
        if(hooks.prepareImages){hooks.prepareImages(hooks.ctx,href.c_str());geometryOffset=eng.chapter().blockCount();}
        else geometryOffset=chaptergeometry::prepareRange(epub,*req.renderer,eng,href,req.imageRendering,geometryOffset,128);
        if(eng.chapter().failed())return fail();
        if(geometryOffset>=eng.chapter().blockCount())phase=Map;
        return Status::Working;
      case Map:
        if(result.fromCache&&!eng.loadPageMap(map)) {char part[240];std::snprintf(part,sizeof(part),"%s.part",map);(void)eng.loadPageMap(part);}
        phase=Finish;return Status::Working;
      case Finish:
        if(eng.chapter().failed())return fail();
        result.ok=true;status=Status::Done;
        LOG_INF("CHLOAD","ready spine=%d cache=%d text=%u map=%d ms=%lu free=%u maxA=%u",req.spineIndex,result.fromCache?1:0,
                unsigned(eng.chapter().textSize()),eng.mapKnownPages(),static_cast<unsigned long>(millis()-started),
                unsigned(ESP.getFreeHeap()),unsigned(ESP.getMaxAllocHeap()));
        return status;
    }
    return fail();
  }
};
Session::Session(const Request& req,const Hooks& hooks){if(casper_memory::allowAllocation())impl_.reset(new(std::nothrow) Impl(req,hooks));}
Session::~Session()=default;
Session::Status Session::step(){return impl_?impl_->step():Status::Failed;}
const Result& Session::result()const {static const Result empty{};return impl_?impl_->result:empty;}
Result loadChapterIr(const Request& req,const Hooks& hooks){
  Session session(req,hooks);
  while(true){if(hooks.shouldAbort&&hooks.shouldAbort(hooks.ctx))return {};
    const auto status=session.step();if(status==Session::Status::Done)return session.result();
    if(status!=Session::Status::Working)return {};
    yield();resetTaskWatchdogIfSubscribed();
  }
}
} // namespace chapterload
