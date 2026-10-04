#include <gtest/gtest.h>
#include "util/PopupTextLayout.h"
#include <string>
static int measure(const char* p){int w=0;while(*p){p+=popuptext::cpBytes(p);w+=8;}return w;}
TEST(PopupTextLayout, ShortMessageUnchanged){
 const auto l=popuptext::wrap("Loading...",300,8,measure);
 ASSERT_EQ(l.count,1u);EXPECT_EQ(l.width,80);EXPECT_FALSE(l.lines[0].ellipsis);
 char b[popuptext::kLineBytes];popuptext::copyLine("Loading...",l.lines[0],b);EXPECT_STREQ(b,"Loading...");
}
TEST(PopupTextLayout, LongStatusFitsEveryOrientation){
 for(int width:{160,240,480,744}){
  const char* text="Chapter preparation failed; place preserved. Choose a chapter to continue.";
  const auto l=popuptext::wrap(text,width,8,measure);
  ASSERT_GT(l.count,0u);EXPECT_LE(l.width,width);
  for(size_t i=0;i<l.count;++i){char b[popuptext::kLineBytes];popuptext::copyLine(text,l.lines[i],b);EXPECT_LE(measure(b),width);}
 }
}
TEST(PopupTextLayout, LongWordAndUtf8AreBounded){
 const std::string text=std::string(1000,'x')+" "+"日本語 Ελληνικά café";
 auto l=popuptext::wrap(text.c_str(),72,8,measure);ASSERT_EQ(l.count,8u);EXPECT_TRUE(l.lines[7].ellipsis);
 l=popuptext::wrap("日本語 Ελληνικά café",24,8,measure);
 for(size_t i=0;i<l.count;++i){char b[popuptext::kLineBytes];popuptext::copyLine("日本語 Ελληνικά café",l.lines[i],b);EXPECT_LE(measure(b),24);}
}
TEST(PopupTextLayout, DegenerateAndMultiline){
 EXPECT_EQ(popuptext::wrap(nullptr,100,8,measure).count,0u);
 EXPECT_EQ(popuptext::wrap("text",0,8,measure).count,0u);
 EXPECT_EQ(popuptext::wrap("text",3,8,measure).count,0u);
 EXPECT_EQ(popuptext::wrap("\n\t\r",100,8,measure).count,0u);
 const auto l=popuptext::wrap(" First\nSecond ",100,8,measure);ASSERT_EQ(l.count,2u);
 char b[popuptext::kLineBytes];popuptext::copyLine(" First\nSecond ",l.lines[1],b);EXPECT_STREQ(b,"Second");
}
