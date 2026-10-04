#pragma once
#include "HalDisplay.h"
struct GfxRenderer {
 bool dark=false,polarity=true;mutable int fast=0,half=0,soft=0,windows=0;
 bool getInvertOnDisplay()const{return dark;}
 bool panelPolarityMatchesInvertFlag()const{return polarity;}
 void displayBuffer(int mode)const{if(mode==HalDisplay::FAST_REFRESH)++fast;else ++half;}
 void displayGrayscaleBase(int)const{++soft;}
 void displayWindow(int,int,int,int)const{++windows;}
 int getScreenWidth()const{return 528;}int getScreenHeight()const{return 792;}
};
