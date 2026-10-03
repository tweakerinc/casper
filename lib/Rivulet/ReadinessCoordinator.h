#pragma once
#include <memory>
#include <string>
#include "BookPageIndex.h"
#include "RivuletEngine.h"
#include "ProgressAnchor.h"
namespace rivulet {
// Owns one bounded background/target engine. The active reader is never lent
// out: navigation becomes visible only after the requested destination exists.
class ReadinessCoordinator {
 public:
  enum class Load { Ready, Empty, Failed, Cancelled, Working };
  enum class Tick { Idle, Working, NavigationReady, NavigationFailed };
  using Loader=Load(*)(void*,RivuletEngine&,int);
  struct Preparation {virtual ~Preparation()=default;virtual Load step()=0;};
  using Factory=std::unique_ptr<Preparation>(*)(void*,RivuletEngine&,int);
  Tick tickPrepared(const GfxRenderer& renderer,int currentSpine,Factory factory,void* ctx,uint32_t now,bool(*abort)());
  bool configure(const std::string& dir,const RenderKey& key,float lineCompression,int spines);
  void request(int spine,int page=0); // page -1 means the verified final page
  void requestAnchor(const ProgressAnchor& anchor);
  void requestFraction(int spine, uint16_t fraction10000);
  void cancelNavigation();
  void release();
  void checkpoint();
  void releaseWorker(){dropWorker();}
  Tick tick(const GfxRenderer& renderer,int currentSpine,Loader loader,void* ctx,uint32_t now,bool(*abort)());
  bool pending()const{return pending_;}
  int destination()const{return requestedSpine_;}
  int requestedPage()const{return requestedPage_;}
  // Caller can move this engine into its active slot after NavigationReady.
  RivuletEngine* readyEngine(){return navigationReady_?worker_.get():nullptr;}
  void consumed();
  std::unique_ptr<RivuletEngine> takeReady(){if(!navigationReady_)return {};auto result=std::move(worker_);consumed();return result;}
  bool recordCurrent(int spine,const RivuletEngine& engine);
  void focus(int spine){focus_=spine;if(index_.valid())index_.focus(spine);probeStage_=0;}
  const BookPageIndex& index()const{return index_;}
  bool workerLoaded()const{return worker_&&loaded_;}
  bool hasWorker()const{return bool(worker_);}
  int workerSpine()const{return workerSpine_;}
 private:
  enum class Goal { Page, Anchor, Fraction };
  Goal goal_=Goal::Page;
  ProgressAnchor anchor_{};
  IrCursor resolvedAnchor_{};
  bool anchorResolved_=false;
  uint16_t fraction_=0;
  Tick advance(const GfxRenderer&,int,Loader,Factory,void*,uint32_t,bool(*)());
  void dropWorker();
  int choose(int current,uint32_t now);
  bool deferred(int spine,uint32_t now)const;
  void failLater(int spine,uint32_t now);
  std::unique_ptr<RivuletEngine> worker_;
  std::unique_ptr<Preparation> preparation_;
  bool loaded_=false;
  BookPageIndex index_;
  std::string dir_;
  RenderKey key_{};
  float lineCompression_=1;
  int spines_=0,focus_=0,workerSpine_=-1,requestedSpine_=-1,requestedPage_=0;
  int lastTitleSpine_=-1;
  int probeStage_=0,crawl_=0,lastSavedKnown_=0,lastActiveSpine_=-1,lastActiveCount_=-1;
  bool lastSavedComplete_=false;
  unsigned failureSlot_=0;
  struct Failure {int spine=-1;uint32_t until=0;} failures_[8];
  bool pending_=false,navigationReady_=false,firstPrepared_=false,lastPrepared_=false;
};
} // namespace rivulet
