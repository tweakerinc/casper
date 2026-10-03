#include "ReadinessCoordinator.h"
#include "NavigationSections.h"
#include <GfxRenderer.h>
#include <Logging.h>
#include <new>
namespace rivulet {
bool ReadinessCoordinator::configure(const std::string& dir,const RenderKey& key,float lc,int spines){
  if(dir==dir_&&key==key_&&spines==spines_)return true;
  release();dir_=dir;key_=key;lineCompression_=lc;spines_=spines;
  probeStage_=crawl_=0;for(auto&f:failures_)f={};
  // A count-ledger failure must not prevent reading. It is derived, optional data.
  if(spines>0&&!dir.empty()){Storage.ensureDirectoryExists(dir.c_str());(void)index_.open(dir.c_str(),key,spines);}
  return spines>0&&!dir.empty();
}
void ReadinessCoordinator::request(int spine,int page){
  if(spine<0||spine>=spines_)return;
  if(worker_&&workerSpine_!=spine)dropWorker();
  goal_=Goal::Page;anchorResolved_=false;requestedSpine_=spine;requestedPage_=page;pending_=true;navigationReady_=false;if(page<0)lastPrepared_=false;
}
void ReadinessCoordinator::requestAnchor(const ProgressAnchor& anchor){
  request(static_cast<int>(anchor.spine),0);
  if(!pending_)return;
  goal_=Goal::Anchor;anchor_=anchor;anchorResolved_=false;
}
void ReadinessCoordinator::requestFraction(int spine,uint16_t fraction10000){
  request(spine,0);if(!pending_)return;
  goal_=Goal::Fraction;fraction_=std::min<uint16_t>(fraction10000,10000);
}
void ReadinessCoordinator::cancelNavigation(){pending_=navigationReady_=false;requestedSpine_=-1;}
void ReadinessCoordinator::checkpoint(){
  if(!worker_||!loaded_||workerSpine_<0||!worker_->hasChapter()||worker_->chapter().failed()||worker_->mapKnownPages()==0)return;
  (void)sectionlabel::save(dir_.c_str(),workerSpine_,worker_->chapter());
  if(worker_->mapKnownPages()==lastSavedKnown_ && worker_->mapComplete()==lastSavedComplete_)return;
  char path[256];
  if(std::snprintf(path,sizeof(path),"%s/s%d_m%u.rvpm%s",dir_.c_str(),workerSpine_,unsigned(key_.pad&15),worker_->mapComplete()?"":".part")>=int(sizeof(path)))return;
  if(worker_->savePageMap(path)){lastSavedKnown_=worker_->mapKnownPages();lastSavedComplete_=worker_->mapComplete();}
}
void ReadinessCoordinator::dropWorker(){checkpoint();preparation_.reset();loaded_=false;worker_.reset();workerSpine_=-1;firstPrepared_=lastPrepared_=false;lastSavedKnown_=0;lastSavedComplete_=false;}
void ReadinessCoordinator::release(){dropWorker();index_.close();dir_.clear();spines_=0;lastTitleSpine_=-1;lastActiveSpine_=lastActiveCount_=-1;cancelNavigation();}
void ReadinessCoordinator::consumed(){preparation_.reset();loaded_=false;worker_.reset();workerSpine_=-1;firstPrepared_=lastPrepared_=false;lastSavedKnown_=0;lastSavedComplete_=false;cancelNavigation();}
bool ReadinessCoordinator::deferred(int s,uint32_t now)const{for(const auto&f:failures_)if(f.spine==s&&static_cast<int32_t>(now-f.until)<0)return true;return false;}
void ReadinessCoordinator::failLater(int s,uint32_t now){failures_[failureSlot_++%8]={s,now+10000U};}
int ReadinessCoordinator::choose(int current,uint32_t now){
  // Bounded number of SD probes per UI loop, regardless of book length.
  for(int i=0;i<8;++i){int s;
    if(probeStage_==0){s=current-1;probeStage_=1;}
    else if(probeStage_==1){s=current+1;probeStage_=2;}
    else {if(spines_<=0)return -1;s=crawl_++%spines_;}
    if(s<0||s>=spines_||s==current||deferred(s,now))continue;
    uint32_t pages;if(index_.read(s,pages))continue;
    return s;
  }return -1;
}
bool ReadinessCoordinator::recordCurrent(int spine,const RivuletEngine& engine){
  if(spine!=lastTitleSpine_&&engine.hasChapter()&&!engine.chapter().failed()){
    (void)sectionlabel::save(dir_.c_str(),spine,engine.chapter());lastTitleSpine_=spine;
  }
  if(!engine.mapComplete()||engine.chapter().failed())return false;
  if(lastActiveSpine_==spine&&lastActiveCount_==engine.mapKnownPages())return true;
  if(!index_.record(spine,engine.mapKnownPages()))return false;
  lastActiveSpine_=spine;lastActiveCount_=engine.mapKnownPages();return true;
}
ReadinessCoordinator::Tick ReadinessCoordinator::tick(const GfxRenderer& renderer,int current,Loader loader,void*ctx,uint32_t now,bool(*abort)()) {
  return advance(renderer,current,loader,nullptr,ctx,now,abort);
}
ReadinessCoordinator::Tick ReadinessCoordinator::tickPrepared(const GfxRenderer& renderer,int current,Factory factory,void*ctx,uint32_t now,bool(*abort)()) {
  return advance(renderer,current,nullptr,factory,ctx,now,abort);
}
ReadinessCoordinator::Tick ReadinessCoordinator::advance(const GfxRenderer& renderer,int current,Loader loader,Factory factory,void*ctx,uint32_t now,bool(*abort)()){
  if((!loader&&!factory)||spines_<=0||navigationReady_||(abort&&abort()))return Tick::Idle;
  if(!worker_){
    const int target=pending_?requestedSpine_:choose(current,now);
    if(target<0)return Tick::Idle;
    if(!casper_memory::allowAllocation()){failLater(target,now);const bool asked=pending_;if(asked)cancelNavigation();return asked?Tick::NavigationFailed:Tick::Idle;}
    worker_.reset(new(std::nothrow) RivuletEngine());
    if(!worker_){failLater(target,now);const bool asked=pending_;if(asked)cancelNavigation();return asked?Tick::NavigationFailed:Tick::Idle;}
    workerSpine_=target;worker_->deferPageCacheWrites(true);worker_->setRenderKey(key_);worker_->setLineCompression(lineCompression_);
    loaded_=false;
    if(factory)preparation_=factory(ctx,*worker_,target);
  }
  if(!loaded_) {
    const int target=workerSpine_;
    const Load loaded=factory?(preparation_?preparation_->step():Load::Failed):loader(ctx,*worker_,target);
    if(loaded==Load::Working)return Tick::Working;
    if(loaded==Load::Empty){
      (void)index_.record(target,0);dropWorker();
      if(pending_){if(goal_!=Goal::Page){cancelNavigation();return Tick::NavigationFailed;}const int step=requestedPage_<0?-1:1;requestedSpine_+=step;
        if(requestedSpine_<0||requestedSpine_>=spines_){cancelNavigation();return Tick::NavigationFailed;}}
      return Tick::Working;
    }
    if(loaded!=Load::Ready||worker_->chapter().failed()){
      const bool failed=pending_&&loaded!=Load::Cancelled;
      dropWorker();if(loaded!=Load::Cancelled)failLater(target,now);
      if(failed)cancelNavigation();
      return failed?Tick::NavigationFailed:Tick::Idle;
    }
    loaded_=true;preparation_.reset();
    // Loading and laying out are separate ticks: return to input polling first.
    return Tick::Working;
  }
  if(pending_&&workerSpine_!=requestedSpine_){dropWorker();return Tick::Working;}
  worker_->setMapAbortCheck(abort);
  struct ClearAbort {RivuletEngine* e;~ClearAbort(){if(e)e->setMapAbortCheck(nullptr);}} guard{worker_.get()};
  bool layoutFailed=false;
  if(pending_&&goal_==Goal::Anchor&&!anchorResolved_) {
    anchorResolved_=anchor_.resolve(worker_->chapter(),resolvedAnchor_);
    if(!anchorResolved_){guard.e->setMapAbortCheck(nullptr);guard.e=nullptr;dropWorker();cancelNavigation();return Tick::NavigationFailed;}
  }
  if(pending_&&goal_==Goal::Fraction&&worker_->mapComplete()) {
    requestedPage_=std::min(worker_->mapKnownPages()-1,static_cast<int>(uint64_t(worker_->mapKnownPages())*fraction_/10000));
    goal_=Goal::Page;
  }
  if(!firstPrepared_){
    if(worker_->goToStart(renderer)){
      firstPrepared_=true;
      // User navigation must paint before optional cache publication. A
      // background first page is stored once, then its strings are released.
      if(!pending_){(void)worker_->flushPageCache();checkpoint();worker_->releasePaintPage();}
    }
    else layoutFailed=!(abort&&abort());
  }else if(pending_&&goal_==Goal::Anchor&&worker_->mapCoversCursor(resolvedAnchor_)){
    if(worker_->resumeAtCursor(renderer,resolvedAnchor_,0)){navigationReady_=true;return Tick::NavigationReady;}
    layoutFailed=!(abort&&abort());
  }else if(pending_&&goal_==Goal::Page&&requestedPage_>=0&&worker_->mapKnownPages()>requestedPage_){
    if(worker_->goToPage(renderer,requestedPage_,0)){navigationReady_=true;return Tick::NavigationReady;}
    layoutFailed=!(abort&&abort());
  }else if(!worker_->mapComplete()){
    const int before=worker_->mapKnownPages();
    const bool progress=worker_->extendPageMap(renderer,1);
    if(worker_->mapKnownPages()-lastSavedKnown_>=8||worker_->mapComplete())checkpoint();
    if(!progress&&before==worker_->mapKnownPages()&&!worker_->mapComplete())layoutFailed=!(abort&&abort());
  }else if(!lastPrepared_){
    // Only one paint-layout at the verified tail. No reverse-pagination guess.
    if(worker_->goToLastPage(renderer,1,false)&&worker_->page().atChapterEnd){
      lastPrepared_=true;
      if(!pending_){(void)worker_->flushPageCache();checkpoint();(void)index_.record(workerSpine_,worker_->mapKnownPages());worker_->releasePaintPage();}
      if(pending_&&requestedPage_<0){navigationReady_=true;return Tick::NavigationReady;}
      if(pending_&&requestedPage_>=worker_->mapKnownPages())layoutFailed=true; // never silently reset to page zero
    }else layoutFailed=!(abort&&abort());
  }else if(!pending_){
    // Guard holds a raw pointer; clear it before releasing the worker.
    guard.e->setMapAbortCheck(nullptr);guard.e=nullptr;
    dropWorker();return Tick::Working;
  }
  if(firstPrepared_&&pending_&&goal_==Goal::Page&&requestedPage_==0){navigationReady_=true;return Tick::NavigationReady;}
  if(layoutFailed||worker_->chapter().failed()){
    const bool failed=pending_;const int target=workerSpine_;
    guard.e->setMapAbortCheck(nullptr);guard.e=nullptr;
    dropWorker();failLater(target,now);if(failed)cancelNavigation();
    return failed?Tick::NavigationFailed:Tick::Idle;
  }
  return Tick::Working;
}
} // namespace rivulet
