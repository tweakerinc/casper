#include <gtest/gtest.h>
#include "Rivulet/ChapterStyles.h"
#include "Rivulet/NavigationSections.h"
#include "Rivulet/StylesheetHead.h"
#include "Rivulet/HtmlToIr.h"
#include <string>
using namespace rivulet;
namespace {
struct View {std::string_view s;size_t limit=512;int read(char*p,size_t n){n=std::min({n,s.size(),limit});if(n)memcpy(p,s.data(),n);s.remove_prefix(n);return int(n);}};
std::string blockText(const ChapterIr& ir,size_t i){const auto b=ir.blocks()[i];std::string s;for(size_t r=b.runBegin;r<b.runBegin+b.runCount;++r){const auto run=ir.runs()[r];s.append(ir.runText(run),run.textLen);}return s;}
std::vector<std::string> textBlocks(const ChapterIr& ir){std::vector<std::string> v;for(size_t i=0;i<ir.blockCount();++i)if(ir.blocks()[i].runCount)v.push_back(blockText(ir,i));return v;}
}
TEST(FieldFormatting, OpaqueDivClassesPreserveParagraphsAndChapterHeading){
  ChapterStyles css;View v{".x {text-align:center;font-size:1.5em;font-weight:bold;margin-bottom:1em;text-indent:0} .p {text-indent:1.2em;text-align:justify}"};ASSERT_TRUE(css.load(v));
  ChapterIr ir;ASSERT_TRUE(HtmlToIr::convert("<body><div class='x'>CHAPTER [1]</div><div class='p'>First paragraph.</div><div class='p'>Next paragraph.</div></body>",ir,false,0,&css));
  const auto blocks=textBlocks(ir);ASSERT_EQ(blocks.size(),3);EXPECT_EQ(blocks[0],"CHAPTER [1]");EXPECT_EQ(blocks[1],"First paragraph.");EXPECT_EQ(blocks[2],"Next paragraph.");
  EXPECT_EQ(ir.blocks()[0].align,Align::Center);EXPECT_TRUE(hasStyleBit(ir.runs()[0].style,RunStyle::Bold));EXPECT_EQ(ir.runs()[0].sizeStep,SizeStep::Plus2);
  EXPECT_EQ(ir.blocks().back().indentEmQ4,19);EXPECT_EQ(ir.blocks().back().align,Align::Justify);
}
TEST(FieldFormatting, InlineWhitespaceAndChunkBoundaryWhitespaceSurvive){
  std::string prose;for(int i=0;i<900;++i)prose+="word ";prose+="end";
  const std::string html="<div>"+prose+" <i>italic</i> regular <span>span</span> tail</div>";
  ChapterIr ir;ASSERT_TRUE(HtmlToIr::convert(html.data(),html.size(),ir));ASSERT_EQ(textBlocks(ir).size(),1);
  EXPECT_EQ(blockText(ir,0),prose+" italic regular span tail");
}
TEST(FieldFormatting, BodyCssInheritanceAndInlineOverride){
  ChapterStyles css;View v{"body {font-style:italic;text-align:center} .x {font-weight:bold} #a {font-weight:normal}"};ASSERT_TRUE(css.load(v));
  ChapterIr ir;ASSERT_TRUE(HtmlToIr::convert("<html><head></head><body><div class='x' id='a' style='text-align:right'>A</div><div>B</div></body></html>",ir,false,0,&css));
  ASSERT_EQ(textBlocks(ir).size(),2);EXPECT_EQ(ir.blocks()[0].align,Align::Right);EXPECT_EQ(ir.blocks()[1].align,Align::Center);
  EXPECT_EQ(ir.runs()[0].style,RunStyle::Italic);EXPECT_EQ(ir.runs()[1].style,RunStyle::Italic);
}
TEST(FieldFormatting, LinkedHiddenVoidAndNestedNodesDoNotSwallowFollowingText){
  ChapterStyles css;View v{".hidden{display:none}"};ASSERT_TRUE(css.load(v));ChapterIr ir;
  ASSERT_TRUE(HtmlToIr::convert("<body><img class='hidden' src='a.jpg'><div class='hidden'><div class='hidden'>secret</div><img src='b.jpg'>secret</div><div>Visible</div></body>",ir,false,0,&css));
  ASSERT_EQ(textBlocks(ir).size(),1);EXPECT_EQ(textBlocks(ir)[0],"Visible");
}
TEST(FieldFormatting, RulesRespectSpecificityAndSourceOrderWithoutGuessingCombinators){
  ChapterStyles css;View v{".x{font-weight:bold} div{font-weight:normal} div.x{font-style:italic} #a{font-weight:normal} p .x{font-size:4em} .x{ text-align:right }"};ASSERT_TRUE(css.load(v));
  auto a=css.resolve("div","x","a");EXPECT_EQ(a.fontWeight,CssFontWeight::Normal);EXPECT_EQ(a.fontStyle,CssFontStyle::Italic);EXPECT_EQ(a.textAlign,CssTextAlign::Right);EXPECT_FALSE(a.defined.fontSize);
}
TEST(FieldFormatting, PunctuationHasSameUnicodeFromEntities){
 ChapterIr ir;ASSERT_TRUE(HtmlToIr::convert("<div>&ldquo;quote&rdquo;&mdash;&hellip;</div>",ir));EXPECT_EQ(blockText(ir,0),"“quote”—…");
}
TEST(FieldFormatting, ExplicitDecorationAndScriptsAreNotLost){
 ChapterStyles css;View v{".s{vertical-align:super;font-size:0.75em} .u{text-decoration:underline line-through}"};ASSERT_TRUE(css.load(v));ChapterIr ir;
 ASSERT_TRUE(HtmlToIr::convert("<p>6<span class='s'>th</span> <span class='u'>both</span></p>",ir,false,0,&css));
 ASSERT_GE(ir.runs().size(),3);EXPECT_TRUE(hasStyleBit(ir.runs()[1].style,RunStyle::Superscript));EXPECT_EQ(ir.runs()[1].sizeStep,SizeStep::Body);
 bool decorated=false;for(const auto r:ir.runs())if(hasStyleBit(r.style,RunStyle::Underline)&&hasStyleBit(r.style,RunStyle::Strikethrough))decorated=true;EXPECT_TRUE(decorated);
}
TEST(FieldFormatting, StylesheetHeadIgnoresFakeLinksAndReadsAcrossInputWindows){
 const std::string html="<html><head><!--"+std::string(8500,'x')+"<link rel='stylesheet' href='fake'>--><link rel='stylesheet' href='../real.css'><style>.a{font-weight:bold}</style></head><body>Real</body></html>";
 HalFile f;ASSERT_TRUE(Storage.openFileForWrite("test","/head-test",f));ASSERT_EQ(f.write(html.data(),html.size()),html.size());f.close();ASSERT_TRUE(Storage.openFileForRead("test","/head-test",f));StylesheetHead head(f);
 unsigned links=0,inlines=0,iterations=0;while(++iterations<1000){auto r=head.step(200);if(r==StylesheetHead::Result::Failed)FAIL();if(r==StylesheetHead::Result::Done)break;if(r==StylesheetHead::Result::Link){++links;EXPECT_EQ(head.value(),"../real.css");}if(r==StylesheetHead::Result::Inline){++inlines;EXPECT_EQ(head.value(),".a{font-weight:bold}");}}
 EXPECT_LT(iterations,1000);EXPECT_EQ(links,1);EXPECT_EQ(inlines,1);
}
TEST(FieldFormatting, BoundedCssParserFailsInsteadOfAllocatingUnboundedRules){
 ChapterStyles css;std::string s;for(unsigned i=0;i<ChapterStyles::kMaxRules+1;++i)s+=".x"+std::to_string(i)+"{font-weight:bold}";View v{s};EXPECT_FALSE(css.load(v));EXPECT_LE(css.size(),ChapterStyles::kMaxRules);
}

TEST(FieldFormatting, SparseNavigationKeepsAllSectionsAndOriginalAnchors){
 NavigationSections sections;int toc[]={0,3,3,7,-1};ASSERT_TRUE(sections.build(10,5,[&](int i){return toc[i];}));
 ASSERT_EQ(sections.size(),12);unsigned originals=0;bool seen[10]{};
 for(size_t i=0;i<sections.size();++i){auto e=sections.at(i);if(e.toc>=0){++originals;EXPECT_EQ(e.spine,toc[e.toc]);}if(e.spine>=0)seen[e.spine]=true;}
 EXPECT_EQ(originals,5);for(bool v:seen)EXPECT_TRUE(v);EXPECT_EQ(sections.at(999).spine,-1);
 EXPECT_FALSE(sections.build(65534,5,[&](int i){return toc[i];}));
}
TEST(FieldFormatting, ChapterLabelsUseVerifiedTextAndSourceScopedCache){
 ChapterIr ch;ASSERT_TRUE(HtmlToIr::convert("<div align='center'>[15]</div><p>Story.</p>",ch));
 ASSERT_TRUE(sectionlabel::save("/sourceA",17,ch));EXPECT_EQ(sectionlabel::read("/sourceA",17),"[15]");EXPECT_TRUE(sectionlabel::read("/sourceB",17).empty());
 char path[256];ASSERT_TRUE(sectionlabel::path("/sourceA",17,path,sizeof(path)));HalFile f;ASSERT_TRUE(Storage.openFileForWrite("test",path,f));ASSERT_EQ(f.write("RVTL\xff" "broken",11),11);f.close();EXPECT_TRUE(sectionlabel::read("/sourceA",17).empty());
}
