#pragma once
#include "EpdFontFamily.h"
#include "BidiUtils.h"
#include <Esp.h>
#include <map>
#include <cstring>
#include "../../../lib/Memory/BoundedUtf8.h"
class GfxRenderer {
 public:
  struct FrameBufferLoan {FrameBufferLoan(GfxRenderer&,bool) {}};
  GfxRenderer() {
    for (int id : {-1128177077,2090520927,-847079762,-209681255,1470095001,-324599973,876380291,426921930,1484141743,652444703,1}) fonts[id] = EpdFontFamily{};
  }
  int height(int id) const { if(id==2090520927)return 18; if(id==-847079762)return 21; if(id==-209681255)return 24; return 16; }
  int getLineHeight(int id, float lc=1.0f) const { return static_cast<int>(height(id)*lc); }
  int getFontAscenderSize(int id) const { return height(id)-4; }
  int getSpaceWidth(int, EpdFontFamily::Style=EpdFontFamily::REGULAR) const { return 4; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int id, const char* s, EpdFontFamily::Style style=EpdFontFamily::REGULAR, int=0) const {
    const char* end=s+std::strlen(s); int w=0;
    while(s<end) { auto cp=casper_memory::nextUtf8(s,end); w+=cp==' '?4:height(id)/2+((style&1)?1:0); }
    return w;
  }
  void ensureSdCardFontReady(int, const char*, uint8_t=15) const {}
  const std::map<int,EpdFontFamily>& getFontMap() const { return fonts; }
  template<class... A> void drawText(A&&...) {}
  template<class... A> void drawLine(A&&...) {}
  template<class... A> void fillRect(A&&...) {}
 private: std::map<int,EpdFontFamily> fonts;
};
