// Host tests for HtmlToIr — the XHTML → ChapterIr converter.
//
// Why these exist: the converter is the one place in the reader where a subtle
// mistake corrupts text silently instead of crashing, and it is about to be
// reworked to stream its input in chunks (so the whole chapter no longer has to
// be resident in RAM). Streaming is only safe if we can prove a chunked parse
// produces byte-identical IR to the one-shot parse, so these tests pin the
// current behaviour first and give the streaming work something to diff against.
//
// The device build gates on heap; on the host we report a large heap so parsing
// runs to completion and the tests measure PARSE correctness, not OOM handling.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ChapterIr.h"
#include "Esp.h"  // host stub — must define ESP before ChapterIr uses it
#include "HtmlToIr.h"
#include "IrFormat.h"
#include "IrTokenizer.h"
#include "PunctEmphasisPolicy.h"

// The firmware links against Arduino's global ESP object; on the host we own it.
EspStub ESP;

namespace {

using rivulet::BlockKind;
using rivulet::ChapterIr;
using rivulet::HtmlToIr;
using rivulet::RunStyle;

ChapterIr convertOrDie(const std::string& html) {
  ChapterIr ir;
  EXPECT_TRUE(HtmlToIr::convert(html.data(), html.size(), ir));
  EXPECT_FALSE(ir.failed()) << "converter reported partial IR on the host (heap is not the limit here)";
  return ir;
}

// Whole-chapter visible text, blocks separated by '\n'. This is the comparison
// used by the boundary sweep: if a chunked parse ever splits a word, drops an
// entity, or duplicates a run, this string changes.
std::string flattenText(const ChapterIr& ir) {
  std::string out;
  for (const auto& b : ir.blocks()) {
    const uint16_t runEnd = static_cast<uint16_t>(b.runBegin + b.runCount);
    for (uint16_t ri = b.runBegin; ri < runEnd && ri < ir.runs().size(); ++ri) {
      out += ir.runString(ir.runs()[ri]);
    }
    out.push_back('\n');
  }
  return out;
}

// Per-run style signature, so a sweep also catches bold/italic leaking across a
// chunk boundary (text can match while styling silently shifts).
std::string flattenStyles(const ChapterIr& ir) {
  std::string out;
  for (const auto& b : ir.blocks()) {
    out += std::to_string(static_cast<int>(b.kind));
    out.push_back(':');
    const uint16_t runEnd = static_cast<uint16_t>(b.runBegin + b.runCount);
    for (uint16_t ri = b.runBegin; ri < runEnd && ri < ir.runs().size(); ++ri) {
      out += std::to_string(static_cast<int>(ir.runs()[ri].style));
      out.push_back(',');
    }
    out.push_back(';');
  }
  return out;
}

int countBlocks(const ChapterIr& ir, const BlockKind kind) {
  int n = 0;
  for (const auto& b : ir.blocks()) {
    if (b.kind == kind) ++n;
  }
  return n;
}

// Representative chapter: prose, styling, nesting, entities, multi-byte UTF-8, a
// heading, a thematic break and a list. Used as the fixture for the sweep.
const char* kSampleChapter =
    "<html><body>"
    "<h1>Chapter One</h1>"
    "<p>The <b>quick</b> brown <i>fox</i> jumps over the lazy dog.</p>"
    "<p>Entities: &amp; &lt; &gt; &quot; &#8212; &#x2014; and &nbsp;spacing.</p>"
    "<p>Unicode: caf\xC3\xA9 na\xC3\xAF ve \xE2\x80\x94 em dash \xE2\x80\xA6 ellipsis.</p>"
    "<p>Nested <b>bold with <i>italic inside</i> still bold</b> then plain.</p>"
    "<hr/>"
    "<ul><li>First item</li><li>Second item</li></ul>"
    "<p>A closing paragraph that is deliberately long enough to span more than a "
    "single internal flush of the text accumulator, so that chunked input has a "
    "realistic chance of splitting it somewhere awkward.</p>"
    "</body></html>";

TEST(HtmlToIr, ParsesParagraphsAndText) {
  const ChapterIr ir = convertOrDie("<p>Hello world.</p><p>Second paragraph.</p>");
  EXPECT_EQ(countBlocks(ir, BlockKind::Paragraph), 2);
  EXPECT_EQ(flattenText(ir), "Hello world.\nSecond paragraph.\n");
}

TEST(HtmlToIr, KeepsSpaceAroundStyleRuns) {
  // Regression: "</i> skill" lost its separator and glued words together.
  const ChapterIr ir = convertOrDie("<p>The <i>Vampire</i> skill fired.</p>");
  EXPECT_EQ(flattenText(ir), "The Vampire skill fired.\n");
}

TEST(HtmlToIr, MarksBoldAndItalicRuns) {
  const ChapterIr ir = convertOrDie("<p>plain <b>bold</b> <i>italic</i></p>");
  bool sawBold = false;
  bool sawItalic = false;
  for (const auto& r : ir.runs()) {
    const auto s = static_cast<uint8_t>(r.style);
    if (s & static_cast<uint8_t>(RunStyle::Bold)) sawBold = true;
    if (s & static_cast<uint8_t>(RunStyle::Italic)) sawItalic = true;
  }
  EXPECT_TRUE(sawBold);
  EXPECT_TRUE(sawItalic);
}

TEST(HtmlToIr, MarksEmTagItalic) {
  const ChapterIr ir = convertOrDie("<p>plain <em>emphasis</em> done</p>");
  bool sawItalic = false;
  for (const auto& r : ir.runs()) {
    if (static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Italic)) sawItalic = true;
  }
  EXPECT_TRUE(sawItalic);
}

TEST(HtmlToIr, StyleDoesNotLeakPastClosingTag) {
  const ChapterIr ir = convertOrDie("<p><b>bold</b>plain</p>");
  ASSERT_GE(ir.runs().size(), 2u);
  const auto last = static_cast<uint8_t>(ir.runs().back().style);
  EXPECT_EQ(last & static_cast<uint8_t>(RunStyle::Bold), 0)
      << "bold leaked past </b> — deep nesting or stack overflow can cause this";
}

TEST(HtmlToIr, DecodesEntities) {
  const ChapterIr ir = convertOrDie("<p>a&amp;b &lt;tag&gt; &#8212; end</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("a&b"), std::string::npos);
  EXPECT_NE(text.find("<tag>"), std::string::npos);
  EXPECT_NE(text.find("\xE2\x80\x94"), std::string::npos) << "&#8212; must decode to a real em dash";
}

TEST(HtmlToIr, PreservesMultiByteUtf8) {
  const ChapterIr ir = convertOrDie("<p>caf\xC3\xA9 \xE2\x80\x94 \xE2\x80\xA6</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("caf\xC3\xA9"), std::string::npos);
  EXPECT_NE(text.find("\xE2\x80\x94"), std::string::npos);
  EXPECT_NE(text.find("\xE2\x80\xA6"), std::string::npos);
}

// Book typography must survive the converter. These characters were previously
// flattened to ASCII ("avoid tofu"), which is what made pages read like a text
// dump; the builtin faces carry all of them.
TEST(HtmlToIr, KeepsBookTypography) {
  const ChapterIr ir = convertOrDie(
      "<p>\xE2\x80\x9CQuoted,\xE2\x80\x9D she said\xE2\x80\x94sharply\xE2\x80\xA6 "
      "it\xE2\x80\x99s fine \xE2\x80\x93 really.</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("\xE2\x80\x9C"), std::string::npos) << "left double quote";
  EXPECT_NE(text.find("\xE2\x80\x9D"), std::string::npos) << "right double quote";
  EXPECT_NE(text.find("\xE2\x80\x99"), std::string::npos) << "apostrophe";
  EXPECT_NE(text.find("\xE2\x80\x94"), std::string::npos) << "em dash";
  EXPECT_NE(text.find("\xE2\x80\x93"), std::string::npos) << "en dash";
  EXPECT_NE(text.find("\xE2\x80\xA6"), std::string::npos) << "ellipsis";
  EXPECT_EQ(text.find("..."), std::string::npos) << "ellipsis must not be expanded to three periods";
}

// Rocky's chord-speech in Project Hail Mary (issue #11). Flattening these to
// ASCII or dropping them is what left `“, ” he says.` on the glass.
TEST(HtmlToIr, KeepsMusicNotes) {
  const ChapterIr ir = convertOrDie("<p>\xE2\x80\x9C\xE2\x99\xAB\xE2\x99\xAB\xE2\x99\xAB,\xE2\x80\x9D he says.</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("\xE2\x99\xAB\xE2\x99\xAB\xE2\x99\xAB,"), std::string::npos)
      << "three beamed eighths plus the grammatical comma must survive";
  EXPECT_NE(text.find("\xE2\x80\x9C"), std::string::npos);
  EXPECT_NE(text.find(" he says."), std::string::npos);
}

TEST(HtmlToIr, KeepsMixedStaffNotes) {
  const ChapterIr ir = convertOrDie("<p>\xE2\x99\xA9 \xE2\x99\xAA \xE2\x99\xAB \xE2\x99\xAC</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("\xE2\x99\xA9"), std::string::npos) << "quarter U+2669";
  EXPECT_NE(text.find("\xE2\x99\xAA"), std::string::npos) << "eighth U+266A";
  EXPECT_NE(text.find("\xE2\x99\xAB"), std::string::npos) << "beamed eighth U+266B";
  EXPECT_NE(text.find("\xE2\x99\xAC"), std::string::npos) << "beamed sixteenth U+266C";
}

// Characters that break layout rather than merely look different are still
// removed / folded.
TEST(HtmlToIr, StripsZeroWidthAndFoldsNoBreakSpace) {
  // Escapes are split with adjacent literals: a following [a-f] digit would
  // otherwise be swallowed into the hex escape (\xADb is not a byte).
  const ChapterIr ir = convertOrDie(
      "<p>a\xC2\xAD"
      "b\xE2\x80\x8B"
      "c\xEF\xBB\xBF"
      "d no\xC2\xA0"
      "break</p>");
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("abcd"), std::string::npos) << "soft hyphen / ZWSP / BOM should be dropped";
  EXPECT_NE(text.find("no break"), std::string::npos) << "nbsp should fold to a plain space";
}

TEST(HtmlToIr, EmitsHorizontalRuleBlock) {
  const ChapterIr ir = convertOrDie("<p>before</p><hr/><p>after</p>");
  EXPECT_EQ(countBlocks(ir, BlockKind::HorizontalRule), 1);
}

TEST(HtmlToIr, EmitsHeadings) {
  const ChapterIr ir = convertOrDie("<h1>Title</h1><h2>Sub</h2><p>Body</p>");
  EXPECT_EQ(countBlocks(ir, BlockKind::Heading1), 1);
  EXPECT_EQ(countBlocks(ir, BlockKind::Heading2), 1);
}

TEST(HtmlToIr, HeadingsDefaultCenter) {
  const ChapterIr ir = convertOrDie("<h2>Introduction</h2><p>Long before Austenmania.</p>");
  ASSERT_FALSE(ir.blocks().empty());
  EXPECT_EQ(ir.blocks().front().kind, BlockKind::Heading2);
  EXPECT_EQ(ir.blocks().front().align, rivulet::Align::Center);
}

TEST(HtmlToIr, HtmlAlignCenterOnParagraph) {
  // Classic engine honored align= even with stylesheets off. v0.1.9 left these
  // titles looking left-aligned (single-word justify).
  const ChapterIr ir = convertOrDie("<p align=\"center\"><b>Introduction</b></p><p>Body.</p>");
  ASSERT_GE(ir.blocks().size(), 1u);
  EXPECT_EQ(ir.blocks().front().kind, BlockKind::Paragraph);
  EXPECT_EQ(ir.blocks().front().align, rivulet::Align::Center);
}

TEST(HtmlToIr, TableCellAlignCenter) {
  const ChapterIr ir = convertOrDie("<table><tr><td align=\"center\">Introduction</td></tr></table>");
  ASSERT_GE(ir.blocks().size(), 1u);
  EXPECT_EQ(ir.blocks().front().align, rivulet::Align::Center);
  EXPECT_NE(flattenText(ir).find("Introduction"), std::string::npos);
}

TEST(HtmlToIr, ChapterOrnamentIsCenteredNotFloated) {
  const ChapterIr ir = convertOrDie(
      "<h2>Introduction</h2><p align=\"center\"><img src=\"images/flourish.png\" alt=\"\"/></p>"
      "<p>Long before Austenmania overtook America.</p>");
  bool sawImage = false;
  for (const auto& b : ir.blocks()) {
    if (b.kind != BlockKind::Image) continue;
    sawImage = true;
    EXPECT_EQ(b.align, rivulet::Align::Center);
    EXPECT_EQ(b.flags & rivulet::kBlockFloatLeft, 0);
    EXPECT_EQ(b.flags & rivulet::kBlockFloatRight, 0);
  }
  EXPECT_TRUE(sawImage);
}

TEST(HtmlToIr, FigleftImageStillFloatsLeft) {
  const ChapterIr ir = convertOrDie(
      "<div class=\"figleft\"><img src=\"images/letter-c.png\" alt=\"C\"/></div>"
      "<p>Alice was beginning to get very tired.</p>");
  bool sawFloat = false;
  for (const auto& b : ir.blocks()) {
    if (b.kind != BlockKind::Image) continue;
    EXPECT_NE(b.flags & rivulet::kBlockFloatLeft, 0);
    sawFloat = true;
  }
  EXPECT_TRUE(sawFloat);
}

TEST(HtmlToIr, ImgAlignLeftFloats) {
  const ChapterIr ir = convertOrDie("<p><img src=\"c.png\" align=\"left\" alt=\"C\"/>Alice.</p>");
  bool sawFloat = false;
  for (const auto& b : ir.blocks()) {
    if (b.kind != BlockKind::Image) continue;
    EXPECT_NE(b.flags & rivulet::kBlockFloatLeft, 0);
    sawFloat = true;
  }
  EXPECT_TRUE(sawFloat);
}

TEST(HtmlToIr, SkipsScriptStyleAndSvg) {
  const ChapterIr ir = convertOrDie(
      "<p>keep</p><script>var x = 'drop';</script><style>p{color:red}</style>"
      "<svg><path d='M0 0'/></svg><p>keep2</p>");
  const std::string text = flattenText(ir);
  EXPECT_EQ(text.find("drop"), std::string::npos);
  EXPECT_EQ(text.find("color"), std::string::npos);
  EXPECT_EQ(text.find("M0 0"), std::string::npos);
  EXPECT_NE(text.find("keep"), std::string::npos);
  EXPECT_NE(text.find("keep2"), std::string::npos);
}

TEST(HtmlToIr, EachListItemIsItsOwnBlock) {
  const ChapterIr ir = convertOrDie("<ul><li>one</li><li>two</li><li>three</li></ul>");
  EXPECT_GE(ir.blockCount(), 3u);
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("one"), std::string::npos);
  EXPECT_NE(text.find("two"), std::string::npos);
  EXPECT_NE(text.find("three"), std::string::npos);
}

TEST(HtmlToIr, SampleChapterIsStable) {
  const ChapterIr ir = convertOrDie(kSampleChapter);
  EXPECT_GT(ir.blockCount(), 5u);
  EXPECT_GT(ir.textSize(), 200u);
  const std::string text = flattenText(ir);
  EXPECT_NE(text.find("Chapter One"), std::string::npos);
  EXPECT_NE(text.find("quick"), std::string::npos);
  EXPECT_NE(text.find("closing paragraph"), std::string::npos);
}

// Converting the same input twice must produce identical IR. This is the
// invariant the streaming boundary sweep will assert against, so prove the
// baseline is deterministic before relying on it.
TEST(HtmlToIr, ConversionIsDeterministic) {
  const ChapterIr a = convertOrDie(kSampleChapter);
  const ChapterIr b = convertOrDie(kSampleChapter);
  EXPECT_EQ(flattenText(a), flattenText(b));
  EXPECT_EQ(flattenStyles(a), flattenStyles(b));
  EXPECT_EQ(a.blockCount(), b.blockCount());
  EXPECT_EQ(a.textSize(), b.textSize());
}

// Feeding the converter a truncated document must not hang, crash, or invent
// text — the streaming version will hit this shape whenever a refill lands mid
// document, so the one-shot parser's behaviour here is the reference.
TEST(HtmlToIr, HandlesTruncatedInputCleanly) {
  const std::string full = kSampleChapter;
  for (size_t cut : {size_t{1}, full.size() / 3, full.size() / 2, full.size() - 1}) {
    ChapterIr ir;
    (void)HtmlToIr::convert(full.data(), cut, ir);
    // Whatever survives must be a prefix-consistent parse: never more text than
    // the full document produced.
    EXPECT_LE(ir.textSize(), convertOrDie(full).textSize()) << "truncated at " << cut;
  }
}

TEST(HtmlToIr, EmptyAndNullInput) {
  ChapterIr ir;
  EXPECT_FALSE(HtmlToIr::convert(nullptr, 10, ir));
  EXPECT_FALSE(HtmlToIr::convert("", 0, ir));
}

TEST(HtmlToIr, CssVerticalAlignMarksSuperscriptAndKeepsOrdinalGlued) {
  // Dungeon Crawler Carl / Jim Butcher ordinals. The suffix must stay a
  // superscript run immediately after the numeral — no extra space — so
  // layout cannot justify "th" out to the right margin.
  const ChapterIr ir = convertOrDie(
      "<p>6<span style=\"font-size:0.75em; margin-left:0.05em; vertical-align:super\">th</span> "
      "Edition</p>");
  EXPECT_EQ(flattenText(ir), "6th Edition\n");
  bool sawSupTh = false;
  bool sawPlainSix = false;
  for (const auto& r : ir.runs()) {
    const std::string t = ir.runString(r);
    const uint8_t s = static_cast<uint8_t>(r.style);
    if (t == "th") {
      EXPECT_NE(s & static_cast<uint8_t>(RunStyle::Superscript), 0);
      EXPECT_EQ(s & static_cast<uint8_t>(RunStyle::Subscript), 0);
      sawSupTh = true;
    }
    if (t == "6") {
      EXPECT_EQ(s & static_cast<uint8_t>(RunStyle::Superscript), 0);
      sawPlainSix = true;
    }
  }
  EXPECT_TRUE(sawSupTh);
  EXPECT_TRUE(sawPlainSix);
}

TEST(HtmlToIr, SupTagMarksSuperscript) {
  const ChapterIr ir = convertOrDie("<p>6<sup>th</sup> Edition</p>");
  EXPECT_EQ(flattenText(ir), "6th Edition\n");
  bool sawSup = false;
  for (const auto& r : ir.runs()) {
    if (ir.runString(r) == "th" && (static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Superscript))) {
      sawSup = true;
    }
  }
  EXPECT_TRUE(sawSup);
}

TEST(HtmlToIr, CssVerticalAlignMarksSubscript) {
  const ChapterIr ir = convertOrDie("<p>H<span style=\"vertical-align:sub\">2</span>O</p>");
  EXPECT_EQ(flattenText(ir), "H2O\n");
  bool sawSub = false;
  for (const auto& r : ir.runs()) {
    if (ir.runString(r) == "2" && (static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Subscript))) {
      sawSub = true;
    }
  }
  EXPECT_TRUE(sawSub);
}

TEST(PunctEmphasis, AsciiOpeningBracket) {
  EXPECT_TRUE(punctemph::isOpeningPunctuation('['));
  EXPECT_TRUE(punctemph::isOpeningPunctuation('('));
  EXPECT_FALSE(punctemph::isOpeningPunctuation(']'));
  EXPECT_FALSE(punctemph::isOpeningPunctuation('1'));
  EXPECT_FALSE(punctemph::isOpeningPunctuation('C'));
}

TEST(PunctEmphasis, TextOnlyOpeningPunct) {
  EXPECT_TRUE(punctemph::textIsOnlyOpeningPunctuation("[", 1));
  EXPECT_TRUE(punctemph::textIsOnlyOpeningPunctuation(" [ ", 3));
  EXPECT_FALSE(punctemph::textIsOnlyOpeningPunctuation("[1]", 3));
  EXPECT_FALSE(punctemph::textIsOnlyOpeningPunctuation("C", 1));
  EXPECT_FALSE(punctemph::textIsOnlyOpeningPunctuation("", 0));
}

TEST(HtmlToIr, DccChapterBracketSpanDoesNotLoneBoldOpeningBracket) {
  // First-letter polyfill: only "[" is wrapped. Must not paint a heavy bracket.
  const ChapterIr ir = convertOrDie("<p>Chapter <span style=\"font-weight:bold\">[</span>1]</p>");
  EXPECT_EQ(flattenText(ir), "Chapter [1]\n");
  for (const auto& r : ir.runs()) {
    if (ir.runString(r).find('[') != std::string::npos) {
      EXPECT_EQ(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0) << ir.runString(r);
    }
  }
}

TEST(HtmlToIr, DccChapterBoldTagOnOpeningBracket) {
  const ChapterIr ir = convertOrDie("<p>Chapter <b>[</b>1]</p>");
  EXPECT_EQ(flattenText(ir), "Chapter [1]\n");
  for (const auto& r : ir.runs()) {
    if (ir.runString(r).find('[') != std::string::npos) {
      EXPECT_EQ(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0) << ir.runString(r);
    }
  }
}

TEST(HtmlToIr, DccChapterSizeSpanOnOpeningBracket) {
  const ChapterIr ir = convertOrDie("<p>Chapter <span style=\"font-size:2em\">[</span>1]</p>");
  EXPECT_EQ(flattenText(ir), "Chapter [1]\n");
  for (const auto& r : ir.runs()) {
    if (ir.runString(r).find('[') != std::string::npos) {
      EXPECT_EQ(r.sizeStep, rivulet::SizeStep::Body) << ir.runString(r);
    }
  }
}

TEST(HtmlToIr, BoldBracketGroupStaysBold) {
  const ChapterIr ir = convertOrDie("<p>See <b>[1]</b> later.</p>");
  EXPECT_EQ(flattenText(ir), "See [1] later.\n");
  bool sawBoldOne = false;
  for (const auto& r : ir.runs()) {
    const std::string t = ir.runString(r);
    if (t.find('1') != std::string::npos) {
      EXPECT_NE(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0) << t;
      sawBoldOne = true;
    }
  }
  EXPECT_TRUE(sawBoldOne);
}

TEST(HtmlToIr, DropCapLetterSpanStaysBold) {
  const ChapterIr ir = convertOrDie("<p><span style=\"font-weight:bold;font-size:2em\">C</span>hapter</p>");
  bool sawBoldC = false;
  for (const auto& r : ir.runs()) {
    if (ir.runString(r) == "C") {
      EXPECT_NE(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0);
      sawBoldC = true;
    }
  }
  EXPECT_TRUE(sawBoldC);
}

TEST(HtmlToIr, DccH1BracketHeadingStaysUniformBold) {
  const ChapterIr ir = convertOrDie("<h1>[ 1 ]</h1>");
  EXPECT_EQ(flattenText(ir), "[ 1 ]\n");
  ASSERT_FALSE(ir.blocks().empty());
  EXPECT_EQ(ir.blocks().front().kind, BlockKind::Heading1);
  for (const auto& r : ir.runs()) {
    EXPECT_NE(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0) << ir.runString(r);
  }
}

TEST(HtmlToIr, DccH1FirstLetterBracketStaysUniformBold) {
  const ChapterIr ir = convertOrDie("<h1><span style=\"font-weight:bold;font-size:2em\">[</span>1]</h1>");
  EXPECT_EQ(flattenText(ir), "[1]\n");
  ASSERT_FALSE(ir.blocks().empty());
  EXPECT_EQ(ir.blocks().front().kind, BlockKind::Heading1);
  for (const auto& r : ir.runs()) {
    EXPECT_NE(static_cast<uint8_t>(r.style) & static_cast<uint8_t>(RunStyle::Bold), 0) << ir.runString(r);
    EXPECT_EQ(r.sizeStep, rivulet::SizeStep::Plus2) << ir.runString(r);
  }
}

std::string tokenText(const ChapterIr& ir, const rivulet::IrTok& t) {
  if (t.runIndex >= ir.runs().size()) return {};
  const auto& run = ir.runs()[t.runIndex];
  if (t.byteOff >= run.textLen) return {};
  const size_t n = std::min<size_t>(t.byteLen, run.textLen - t.byteOff);
  return std::string(ir.runText(run) + t.byteOff, n);
}

TEST(HtmlToIr, TokenizerDoesNotInsertGapBetweenNumeralAndOrdinal) {
  const ChapterIr ir = convertOrDie("<p>6<span style=\"vertical-align:super\">th</span> Edition</p>");
  ASSERT_FALSE(ir.blocks().empty());
  const auto& b = ir.blocks().front();
  std::vector<rivulet::IrTok> toks;
  rivulet::tokenizeRuns(ir, b.runBegin, b.runCount, b.runBegin, 0, toks);

  std::vector<std::string> words;
  std::vector<bool> spaces;
  words.reserve(toks.size());
  spaces.reserve(toks.size());
  for (const auto& t : toks) {
    words.push_back(tokenText(ir, t));
    spaces.push_back(t.space);
  }

  // "6" then "th" with no space token between them, then a real space, then
  // "Edition". A synthetic gap would show up as an extra space=true token
  // (often byteLen 0) between 6 and th.
  auto itSix = std::find(words.begin(), words.end(), "6");
  ASSERT_NE(itSix, words.end());
  const size_t i = static_cast<size_t>(itSix - words.begin());
  ASSERT_LT(i + 1, words.size());
  EXPECT_FALSE(spaces[i]);
  EXPECT_EQ(words[i + 1], "th");
  EXPECT_FALSE(spaces[i + 1]);
  ASSERT_LT(i + 2, words.size());
  EXPECT_TRUE(spaces[i + 2]);
}

TEST(HtmlToIr, TokenizerKeepsRealSpaceAfterStyleRun) {
  const ChapterIr ir = convertOrDie("<p>The <i>Vampire</i> skill fired.</p>");
  ASSERT_FALSE(ir.blocks().empty());
  const auto& b = ir.blocks().front();
  std::vector<rivulet::IrTok> toks;
  rivulet::tokenizeRuns(ir, b.runBegin, b.runCount, b.runBegin, 0, toks);

  std::vector<std::string> seq;
  seq.reserve(toks.size());
  for (const auto& t : toks) seq.push_back(t.space ? " " : tokenText(ir, t));
  // Exactly one space between Vampire and skill — not a synthetic extra.
  std::string joined;
  for (const auto& s : seq) joined += s;
  EXPECT_EQ(joined, "The Vampire skill fired.");
  int vampireToSkillSpaces = 0;
  bool afterVampire = false;
  for (size_t i = 0; i < seq.size(); ++i) {
    if (seq[i] == "Vampire")
      afterVampire = true;
    else if (afterVampire && seq[i] == "skill")
      break;
    else if (afterVampire && seq[i] == " ")
      ++vampireToSkillSpaces;
  }
  EXPECT_EQ(vampireToSkillSpaces, 1);
}

}  // namespace
