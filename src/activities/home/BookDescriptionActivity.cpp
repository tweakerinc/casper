#include "BookDescriptionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/HtmlToPlainText.h"

namespace {

bool isTagStart(const std::string& input, const size_t pos) {
  if (pos + 1 >= input.size()) return false;
  const unsigned char next = input[pos + 1];
  return next == '/' || next == '!' || next == '?' || std::isalpha(next);
}

std::string tagName(const std::string& input, size_t start, const size_t end) {
  while (start < end && (input[start] == '/' || std::isspace(static_cast<unsigned char>(input[start])))) {
    ++start;
  }
  const size_t nameStart = start;
  while (start < end && std::isalpha(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  std::string name = input.substr(nameStart, start - nameStart);
  for (char& c : name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return name;
}

bool looksLikeHtml(const std::string& s) {
  // Real tags, or Calibre-style entity-encoded HTML inside dc:description.
  return (s.find('<') != std::string::npos && s.find('>') != std::string::npos) ||
         (s.find("&lt;") != std::string::npos || s.find("&LT;") != std::string::npos);
}

EpdFontFamily::Style styleFromFlags(const bool bold, const bool italic) {
  if (bold && italic) return EpdFontFamily::BOLD_ITALIC;
  if (bold) return EpdFontFamily::BOLD;
  if (italic) return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}

// True advance for a run. getTextWidth() uses glyph *bitmap* bounds, so a bare
// space (width=0, advance>0) measures as 0 — which used to collapse every gap
// to 1px and made description text look glued together. Use cursor advance.
int measureRunWidth(const GfxRenderer& renderer, const int fontId, const BookDescriptionActivity::Run& run) {
  if (run.text.empty()) return 0;

  // Pure whitespace token (from appendWordTokens).
  bool onlySpaces = true;
  int spaceCount = 0;
  for (const unsigned char c : run.text) {
    if (c == ' ' || c == '\t') {
      ++spaceCount;
    } else {
      onlySpaces = false;
      break;
    }
  }
  if (onlySpaces && spaceCount > 0) {
    int spaceW = renderer.getSpaceWidth(fontId, run.style);
    if (spaceW <= 0) {
      spaceW = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
    }
    // Last-resort fallback: ~0.25em of the font's line height.
    if (spaceW <= 0) {
      spaceW = std::max(3, renderer.getLineHeight(fontId) / 4);
    }
    return spaceCount * spaceW;
  }

  // Mixed or word run: sum word advances + space advances so embedded spaces
  // (if any) still contribute real inter-word width.
  int total = 0;
  size_t i = 0;
  while (i < run.text.size()) {
    const unsigned char c = static_cast<unsigned char>(run.text[i]);
    if (c == ' ' || c == '\t') {
      size_t j = i;
      while (j < run.text.size()) {
        const unsigned char cj = static_cast<unsigned char>(run.text[j]);
        if (cj != ' ' && cj != '\t') break;
        ++j;
      }
      int spaceW = renderer.getSpaceWidth(fontId, run.style);
      if (spaceW <= 0) spaceW = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
      if (spaceW <= 0) spaceW = std::max(3, renderer.getLineHeight(fontId) / 4);
      total += static_cast<int>(j - i) * spaceW;
      i = j;
      continue;
    }
    size_t j = i;
    while (j < run.text.size()) {
      const unsigned char cj = static_cast<unsigned char>(run.text[j]);
      if (cj == ' ' || cj == '\t') break;
      ++j;
    }
    const std::string word = run.text.substr(i, j - i);
    total += renderer.getTextAdvanceX(fontId, word.c_str(), run.style);
    i = j;
  }
  return total;
}

// UTF-8 whitespace that should act like a normal breakable space in descriptions
// (Calibre loves &nbsp; → U+00A0; also thin/hair/figure spaces).
bool isBreakableSpaceUtf8(const std::string& s, size_t i, size_t& byteLen) {
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 == ' ' || c0 == '\t') {
    byteLen = 1;
    return true;
  }
  // U+00A0 NO-BREAK SPACE → C2 A0
  if (c0 == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
    byteLen = 2;
    return true;
  }
  // U+2000–U+200A (en/em/thin/hair…), U+202F NNBSP → E2 80 xx; U+205F → E2 81 9F
  if (c0 == 0xE2 && i + 2 < s.size()) {
    const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
    if (c1 == 0x80 && ((c2 >= 0x80 && c2 <= 0x8A) || c2 == 0xAF)) {
      byteLen = 3;
      return true;
    }
    if (c1 == 0x81 && c2 == 0x9F) {
      byteLen = 3;
      return true;
    }
  }
  byteLen = 0;
  return false;
}

// Tokenize into words and separate space runs (spaces never glued away).
void appendWordTokens(const std::string& text, const EpdFontFamily::Style style,
                      std::vector<BookDescriptionActivity::Run>& out) {
  size_t i = 0;
  while (i < text.size()) {
    size_t spLen = 0;
    if (isBreakableSpaceUtf8(text, i, spLen)) {
      size_t j = i;
      size_t len = 0;
      while (j < text.size() && isBreakableSpaceUtf8(text, j, len)) {
        j += len;
      }
      // Collapse to a single space token — width from getSpaceWidth at measure time.
      out.push_back({" ", style});
      i = j;
      continue;
    }
    if (text[i] == '\n') {
      ++i;
      continue;
    }
    size_t j = i;
    size_t len = 0;
    while (j < text.size() && text[j] != '\n' && !isBreakableSpaceUtf8(text, j, len)) {
      // Advance one UTF-8 codepoint (or byte if invalid).
      const unsigned char c = static_cast<unsigned char>(text[j]);
      if ((c & 0x80) == 0) {
        ++j;
      } else if ((c & 0xE0) == 0xC0 && j + 1 < text.size()) {
        j += 2;
      } else if ((c & 0xF0) == 0xE0 && j + 2 < text.size()) {
        j += 3;
      } else if ((c & 0xF8) == 0xF0 && j + 3 < text.size()) {
        j += 4;
      } else {
        ++j;
      }
    }
    if (j > i) {
      out.push_back({text.substr(i, j - i), style});
    }
    i = j;
  }
}

// Map unicode/HTML spaces to ASCII so layout + measurement stay consistent.
std::string normalizeDescriptionSpaces(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size();) {
    size_t spLen = 0;
    if (isBreakableSpaceUtf8(input, i, spLen)) {
      out.push_back(' ');
      i += spLen;
      continue;
    }
    out.push_back(input[i++]);
  }
  return out;
}

}  // namespace

void BookDescriptionActivity::wrapParagraph(const std::vector<Run>& runs) {
  if (runs.empty()) return;

  Line line;
  int lineW = 0;

  auto flushLine = [&] {
    if (!line.runs.empty()) {
      // Trim trailing space tokens so wrap edges stay clean.
      while (!line.runs.empty() && line.runs.back().text == " ") {
        lineW -= measureRunWidth(renderer, fontId, line.runs.back());
        line.runs.pop_back();
      }
      if (!line.runs.empty()) {
        lines.push_back(std::move(line));
      }
      line = Line{};
      lineW = 0;
    }
  };

  for (const Run& token : runs) {
    if (token.text.empty()) continue;
    const int tw = measureRunWidth(renderer, fontId, token);

    // Skip leading spaces on a new line.
    if (line.runs.empty() && token.text == " ") continue;

    if (tw > textWidth && line.runs.empty() && token.text != " ") {
      // Hard-break overlong token.
      std::string rest = token.text;
      while (!rest.empty()) {
        size_t fit = 1;
        while (fit < rest.size()) {
          const std::string cand = rest.substr(0, fit + 1);
          const Run probe{cand, token.style};
          if (measureRunWidth(renderer, fontId, probe) > textWidth) break;
          ++fit;
        }
        lines.push_back(Line{{{rest.substr(0, fit), token.style}}, false});
        rest.erase(0, fit);
      }
      continue;
    }

    if (!line.runs.empty() && lineW + tw > textWidth) {
      flushLine();
      if (token.text == " ") continue;
    }
    line.runs.push_back(token);
    lineW += tw;
  }
  flushLine();
}

void BookDescriptionActivity::layoutFromHtml(const std::string& raw) {
  lines.clear();

  // Calibre often entity-encodes the whole HTML blob (&lt;p&gt;...). Decode first
  // so tags are real, then style-parse. Safe if already decoded (no &lt; left).
  std::string html = raw;
  if (html.find("&lt;") != std::string::npos || html.find("&LT;") != std::string::npos ||
      html.find("&#") != std::string::npos || html.find("&nbsp;") != std::string::npos ||
      html.find("&NBSP;") != std::string::npos) {
    html = decodeHtmlEntities(html);
  }
  // &nbsp; / U+00A0 etc. → ASCII space so word-split + space advance work.
  html = normalizeDescriptionSpaces(html);

  if (!looksLikeHtml(html) && html.find('<') == std::string::npos) {
    // True plain text.
    const std::string plain = htmlToPlainText(html);
    size_t pos = 0;
    while (pos <= plain.size()) {
      const size_t nl = plain.find('\n', pos);
      const std::string row =
          plain.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (row.empty()) {
        if (!lines.empty() && !lines.back().isParagraphGap) {
          lines.push_back(Line{{}, true});
        }
      } else {
        std::vector<Run> runs;
        appendWordTokens(row, EpdFontFamily::REGULAR, runs);
        wrapParagraph(runs);
      }
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    return;
  }

  bool bold = false;
  bool italic = false;
  std::vector<Run> paraRuns;
  std::string pending;

  auto flushPending = [&] {
    if (pending.empty()) return;
    appendWordTokens(pending, styleFromFlags(bold, italic), paraRuns);
    pending.clear();
  };

  auto endParagraph = [&] {
    flushPending();
    if (!paraRuns.empty()) {
      wrapParagraph(paraRuns);
      paraRuns.clear();
      lines.push_back(Line{{}, true});
    }
  };

  for (size_t i = 0; i < html.size();) {
    if (html[i] == '<' && isTagStart(html, i)) {
      const size_t close = html.find('>', i + 1);
      if (close == std::string::npos) {
        pending.push_back(html[i++]);
        continue;
      }
      flushPending();
      const bool closing = (i + 1 < html.size() && html[i + 1] == '/');
      const std::string name = tagName(html, i + 1, close);

      if (name == "p" || name == "div" || name == "tr" || name == "h1" || name == "h2" || name == "h3" ||
          name == "h4" || name == "h5" || name == "h6") {
        endParagraph();
        if (!closing && (name == "h1" || name == "h2" || name == "h3")) {
          bold = true;
        }
        if (closing && (name == "h1" || name == "h2" || name == "h3")) {
          bold = false;
        }
      } else if (name == "br" || name == "li") {
        // Soft line break within flow: end current line group as a paragraph row.
        endParagraph();
      } else if (name == "b" || name == "strong") {
        bold = !closing;
      } else if (name == "i" || name == "em") {
        italic = !closing;
      }
      i = close + 1;
      continue;
    }

    if (html[i] == '&') {
      const size_t semi = html.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 16) {
        const std::string slice = html.substr(i, semi - i + 1);
        const std::string decoded = decodeHtmlEntities(slice);
        if (!decoded.empty() && decoded != slice) {
          pending += decoded;
          i = semi + 1;
          continue;
        }
      }
    }

    const char c = html[i++];
    if (c == '\r') continue;
    if (c == '\n' || c == '\t') {
      pending.push_back(' ');
    } else {
      pending.push_back(c);
    }
  }
  endParagraph();

  while (!lines.empty() && lines.back().isParagraphGap) {
    lines.pop_back();
  }
}

int BookDescriptionActivity::rowHeight(const Line& line) const {
  if (line.isParagraphGap) return paragraphGap;
  return lineHeight;
}

void BookDescriptionActivity::layoutTitleBlock(const int pageWidth) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Prefer a large serif so the title reads at least as boldly as body text.
  titleFontId = BITTER_18_FONT_ID;
  if (renderer.getLineHeight(titleFontId) <= 0) {
    titleFontId = BITTER_16_FONT_ID;
  }
  if (renderer.getLineHeight(titleFontId) <= 0) {
    titleFontId = UI_12_FONT_ID;
  }

  const char* headerText = title.empty() ? tr(STR_SYNOPSIS) : title.c_str();
  const int sidePad = metrics.contentSidePadding;
  // Full content width so long titles wrap cleanly (up to two lines).
  const int titleMaxW = std::max(40, pageWidth - sidePad * 2);
  constexpr int kTitleMaxLines = 2;
  titleLines = renderer.wrappedText(titleFontId, headerText, titleMaxW, kTitleMaxLines, EpdFontFamily::BOLD);
  if (titleLines.empty()) {
    titleLines.push_back(headerText);
  }

  const int titleLineH = renderer.getLineHeight(titleFontId);
  // Sit title under battery/clock row (same band drawHeader uses for chrome).
  const int chromeBottom =
      metrics.topPadding + BaseTheme::kTopChromeBatteryY + metrics.batteryHeight + 10;
  const int titleY = std::max(metrics.topPadding + 4, chromeBottom);
  const int titleBlockH = static_cast<int>(titleLines.size()) * titleLineH +
                          std::max(0, static_cast<int>(titleLines.size()) - 1) * 1;
  titleBlockBottom = titleY + titleBlockH + metrics.verticalSpacing;
}

void BookDescriptionActivity::drawTitleBlock(const int pageWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int titleMaxW = std::max(40, pageWidth - sidePad * 2);
  const int titleLineH = renderer.getLineHeight(titleFontId);
  const int chromeBottom =
      metrics.topPadding + BaseTheme::kTopChromeBatteryY + metrics.batteryHeight + 10;
  int titleY = std::max(metrics.topPadding + 4, chromeBottom);

  for (const auto& line : titleLines) {
    // Center each line so long titles fill the width without looking left-heavy.
    const int tw = renderer.getTextWidth(titleFontId, line.c_str(), EpdFontFamily::BOLD);
    const int tx = sidePad + std::max(0, (titleMaxW - tw) / 2);
    renderer.drawText(titleFontId, tx, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineH + 1;
  }
}

void BookDescriptionActivity::rebuildLines() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) {
    fontId = UI_12_FONT_ID;
  }

  const int baseLineH = std::max(1, renderer.getLineHeight(fontId));
  const float spacing = SETTINGS.getReaderLineCompression();
  lineHeight = std::max(baseLineH, static_cast<int>(baseLineH * spacing + 0.5f));
  paragraphGap = SETTINGS.extraParagraphSpacing ? lineHeight : std::max(lineHeight / 2, baseLineH / 3);

  const int sidePad = metrics.contentSidePadding + static_cast<int>(SETTINGS.screenMargin);
  const int scrollReserve = metrics.scrollBarWidth + metrics.scrollBarRightOffset + 4;

  layoutTitleBlock(pageWidth);
  contentTop = titleBlockBottom;
  bodyHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  textLeft = sidePad;
  textWidth = std::max(40, pageWidth - sidePad * 2 - scrollReserve);
  visibleLines = std::max(1, bodyHeight / lineHeight);

  if (body.empty()) {
    lines.clear();
    lines.push_back(Line{{{tr(STR_NO_DESCRIPTION), EpdFontFamily::REGULAR}}, false});
  } else {
    layoutFromHtml(body);
  }

  {
    int used = 0;
    int count = 0;
    for (int i = scrollLine; i < static_cast<int>(lines.size()); ++i) {
      const int h = rowHeight(lines[i]);
      if (used + h > bodyHeight) break;
      used += h;
      ++count;
    }
    if (count > 0) visibleLines = count;
  }

  if (scrollLine > 0 && scrollLine + visibleLines > static_cast<int>(lines.size())) {
    scrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  }
}

int BookDescriptionActivity::pageStep() const { return std::max(1, visibleLines - 1); }

void BookDescriptionActivity::drawScrollBar(const int totalLines) const {
  if (totalLines <= visibleLines || bodyHeight <= 8) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int barW = std::max(3, metrics.scrollBarWidth);
  const int trackX = pageWidth - metrics.scrollBarRightOffset - barW;
  const int trackH = bodyHeight;
  const int thumbH = std::max(10, trackH * visibleLines / totalLines);
  const int maxScroll = std::max(1, totalLines - visibleLines);
  const int thumbY = contentTop + (trackH - thumbH) * scrollLine / maxScroll;
  renderer.drawLine(trackX + barW / 2, contentTop, trackX + barW / 2, contentTop + trackH - 1, true);
  renderer.fillRect(trackX, thumbY, barW, thumbH, true);
}

void BookDescriptionActivity::onEnter() {
  Activity::onEnter();
  scrollLine = 0;
  rebuildLines();
  requestUpdate();
}

void BookDescriptionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int total = static_cast<int>(lines.size());
  const int step = pageStep();

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    if (scrollLine > 0) {
      scrollLine = std::max(0, scrollLine - step);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    if (scrollLine + visibleLines < total) {
      scrollLine = std::min(total - visibleLines, scrollLine + step);
      if (scrollLine < 0) scrollLine = 0;
      requestUpdate();
    }
    return;
  }
}

void BookDescriptionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  // Top chrome only (battery/clock) — book title is drawn larger below.
  const int chromeH =
      BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, chromeH}, nullptr);

  rebuildLines();
  drawTitleBlock(pageWidth);

  const int total = static_cast<int>(lines.size());
  int y = contentTop;
  for (int i = scrollLine; i < total; ++i) {
    const Line& line = lines[i];
    const int h = rowHeight(line);
    if (y + h > contentTop + bodyHeight) break;

    if (!line.isParagraphGap) {
      int x = textLeft;
      for (const Run& run : line.runs) {
        if (run.text.empty()) continue;
        const int advance = measureRunWidth(renderer, fontId, run);
        // Space tokens are advance-only (space glyphs have empty bitmaps).
        if (run.text != " ") {
          renderer.drawText(fontId, x, y, run.text.c_str(), true, run.style);
        }
        x += advance;
      }
    }
    y += h;
  }

  int painted = 0;
  int used = 0;
  for (int i = scrollLine; i < total; ++i) {
    const int h = rowHeight(lines[i]);
    if (used + h > bodyHeight) break;
    used += h;
    ++painted;
  }
  if (painted > 0) visibleLines = painted;

  drawScrollBar(total);

  const bool canUp = scrollLine > 0;
  const bool canDown = scrollLine + visibleLines < total;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", canUp ? tr(STR_DIR_UP) : "", canDown ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
