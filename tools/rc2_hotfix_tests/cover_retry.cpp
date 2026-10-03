#include "CoverRenderPolicy.h"
#include <cassert>
#include <iostream>
int main(){
 using thumbcache::DiskThumb;
 unsigned checks=0;auto require=[&](bool v){assert(v);++checks;};
 // Delayed first generation is still eligible; no source failure occurred.
 require(coverrender::retryGenerationPass(false,false));
 require(coverrender::retryGenerationPass(false,true));
 require(coverrender::retryGenerationPass(true,false));
 require(!coverrender::retryGenerationPass(true,true));
 require(!coverrender::settleWhenNoCoverWork(false,false));
 require(!coverrender::settleWhenNoCoverWork(false,true));
 require(!coverrender::settleWhenNoCoverWork(true,true));
 require(coverrender::settleWhenNoCoverWork(true,false));
 // The logged failure must not run its decoder eight times per Home entry.
 unsigned attempts=0;bool pending=true;
 while(pending){++attempts;pending=coverrender::retryGenerationPass(true,true)&&coverrender::keepRetrying(attempts,DiskThumb::Missing,false);}
 require(attempts==1);
 // Valid covers, existing fallbacks and waiting-for-shell rules are retained.
 require(!coverrender::generateHero(DiskThumb::Hero));
 require(!coverrender::generateHero(DiskThumb::Unverified));
 require(coverrender::generateHero(DiskThumb::Fallback));
 require(coverrender::generateHero(DiskThumb::Missing));
 require(!coverrender::keepRetrying(1,DiskThumb::Hero,false));
 require(coverrender::keepRetrying(1,DiskThumb::Missing,false));
 require(coverrender::keepRetrying(1,DiskThumb::Fallback,true));
 require(!coverrender::keepRetrying(8,DiskThumb::Missing,false));
 require(!coverrender::settleMissingCover(1,true));
 require(coverrender::settleMissingCover(8,true));
 require(coverrender::paintWhenHeroArrives());
 std::cout<<checks<<" cover retry policy checks passed (not a decoder or panel test)\n";
}
