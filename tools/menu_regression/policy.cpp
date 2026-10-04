#include "util/UiGhostPolicy.h"
#include "util/PopupTextLayout.h"
#include <cassert>
#include <cstdio>
#include <string>
static int width(const char* p){int w=0;while(*p){p+=popuptext::cpBytes(p);w+=8;}return w;}
int main(){
 for(bool x3:{false,true})for(bool dark:{false,true}){
  gpio.x3=x3;GfxRenderer r;r.dark=dark;UiGhostPolicy::clearHardScrub();
  UiGhostPolicy::displayFastFull(r,true);const int baseline=r.soft;
  for(int i=0;i<12;++i)UiGhostPolicy::displayFastFull(r,false);
  assert(r.fast==13);assert(r.soft==baseline);assert(r.soft==(x3&&!dark?1:0));assert(!r.half);assert(!r.windows);
  UiGhostPolicy::requestHardScrub();UiGhostPolicy::displayFastFull(r,false);assert(r.half==1);
  r.polarity=false;UiGhostPolicy::displayFastFull(r,false);assert(r.half==2);
 }
 const char* message="Chapter preparation failed; place preserved. Select a chapter to continue.";
 for(int w:{24,80,160,240,480,744}){
  const auto l=popuptext::wrap(message,w,8,width);assert(l.count>0&&l.count<=8&&l.width<=w);
  for(size_t i=0;i<l.count;++i){char b[popuptext::kLineBytes];popuptext::copyLine(message,l.lines[i],b);assert(width(b)<=w);}
 }
 const auto shortLine=popuptext::wrap("Loading...",300,8,width);assert(shortLine.count==1&&shortLine.width==80);
 for(int w=1;w<120;++w){
  const char* unicode="日本語 Ελληνικά café with multiple words";const auto l=popuptext::wrap(unicode,w,8,width);
  for(size_t i=0;i<l.count;++i){char b[popuptext::kLineBytes];popuptext::copyLine(unicode,l.lines[i],b);assert(width(b)<=w);}
 }
 assert(popuptext::wrap(nullptr,100,8,width).count==0);
 assert(popuptext::wrap("text",0,8,width).count==0);
 assert(popuptext::wrap("text",3,8,width).count==0);
 const std::string longword(10000,'x');const auto l=popuptext::wrap(longword.c_str(),80,8,width);assert(l.count==8&&l.lines.back().ellipsis);
 std::puts("PASS production UI refresh policy and allocation-free popup wrapping; panel/fonts mocked.");
}
