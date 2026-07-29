#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HtmlToPlainText.h"

namespace {

constexpr int kPopupMarginX = 18;
constexpr int kPopupPadX = 14;
constexpr int kPopupPadY = 12;
constexpr int kHeaderLineGap = 4;
constexpr int kTitleBodyGap = 10;
constexpr int kCorner = 10;
constexpr int kMaxBodyLines = 64;

// StarDict type-code bytes that may prefix a definition when sametypesequence is absent.
bool isStarDictTypeCode(const unsigned char c) {
  // Common: m=plain, h=html, g=pango, x=xdxf, y=pinyin, k=kingsoft, w=media, t=phonetic, etc.
  switch (c) {
    case 'm':
    case 'l':
    case 'g':
    case 't':
    case 'x':
    case 'y':
    case 'k':
    case 'w':
    case 'h':
    case 'r':
    case 'W':
    case 'P':
    case 'X':
      return true;
    default:
      return false;
  }
}

void stripLeadingTypeCodes(std::string& text) {
  // Drop a leading type code if the next byte looks like content start.
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (isStarDictTypeCode(c) && i + 1 < text.size()) {
      const unsigned char n = static_cast<unsigned char>(text[i + 1]);
      // Type code is usually followed by letter, digit, '<', '/', '[', or whitespace.
      if (std::isalnum(n) || n == '<' || n == '/' || n == '[' || n == '{' || n == ' ' || n == '\n' || n == '\t') {
        i++;
        continue;
      }
    }
    break;
  }
  if (i > 0) text.erase(0, i);
}

// Map common fancy unicode punctuation to ASCII so SD-missing glyphs don't show as boxes.
void replaceFancyUnicode(std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      i++;
      continue;
    }
    // Multi-byte UTF-8 sequences we care about
    if (c == 0xE2 && i + 2 < text.size()) {
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      // En dash U+2013, em dash U+2014
      if (b1 == 0x80 && (b2 == 0x93 || b2 == 0x94)) {
        out.push_back('-');
        i += 3;
        continue;
      }
      // Curly single quotes U+2018/2019
      if (b1 == 0x80 && (b2 == 0x98 || b2 == 0x99)) {
        out.push_back('\'');
        i += 3;
        continue;
      }
      // Curly double quotes U+201C/201D
      if (b1 == 0x80 && (b2 == 0x9C || b2 == 0x9D)) {
        out.push_back('"');
        i += 3;
        continue;
      }
      // Ellipsis U+2026
      if (b1 == 0x80 && b2 == 0xA6) {
        out.append("...");
        i += 3;
        continue;
      }
      // Bullet U+2022
      if (b1 == 0x80 && b2 == 0xA2) {
        out.push_back('*');
        i += 3;
        continue;
      }
    }
    // Copy other multi-byte sequences as-is (may still fail to glyph; IPA stripped later if needed)
    out.push_back(text[i++]);
  }
  text.swap(out);
}

// Drop non-printable control bytes; keep \n and tab-as-space.
void stripNonPrintable(std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\n') {
      out.push_back('\n');
    } else if (c == '\t') {
      out.push_back(' ');
    } else if (c < 0x20 || c == 0x7F) {
      continue;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  text.swap(out);
}

// Lowercase first token of a grammar line (POS / gender tags from bilingual packs).
std::string grammarToken(const std::string& line) {
  std::string low;
  low.reserve(line.size());
  for (char c : line) {
    low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  while (!low.empty() && (low.back() == '.' || low.back() == ' ')) low.pop_back();
  size_t sp = low.find(' ');
  return sp == std::string::npos ? low : low.substr(0, sp);
}

// Spanish gender / number tags — shown italic (not numbered, not bold POS).
bool isGenderLabel(const std::string& line) {
  std::string s = line;
  // Strip style markers / indent used in the formatted body.
  while (!s.empty() && (s[0] == '~' || s[0] == ' ')) s.erase(0, 1);
  const std::string token = grammarToken(s);
  return token == "f" || token == "m" || token == "mf" || token == "m/f" || token == "f/m" || token == "pl" ||
         token == "sg" || token == "feminine" || token == "masculine" || token == "neuter" || token == "masc./fem." ||
         token == "plural" || token == "singular";
}

bool isPosLabel(const std::string& line) {
  // Part-of-speech labels (bold). Gender is handled separately via isGenderLabel.
  static constexpr const char* kPos[] = {
      "noun",       "verb",      "adjective", "adverb",     "pronoun",    "preposition", "conjunction",
      "interjection", "determiner", "numeral", "particle",  "article",    "proper",      "prop",
      "adj",        "adv",       "n",         "v",          "prep",       "conj",        "interj",
      "phrase",     "idiom",     "prefix",    "suffix",     "abbreviation", "symbol",     "misc", "other",
  };
  std::string s = line;
  while (!s.empty() && s[0] == ' ') s.erase(0, 1);
  if (isGenderLabel(s)) return false;
  const std::string token = grammarToken(s);
  for (const char* p : kPos) {
    if (token == p) {
      return true;
    }
  }
  return false;
}

// Expand short pack tags so the e-ink card is readable:
//   f → feminine, m → masculine, v → verb, adj → adjective, …
std::string expandGrammarLabel(const std::string& line) {
  const std::string token = grammarToken(line);
  if (token == "f" || token == "f.") return "feminine";
  if (token == "m" || token == "m.") return "masculine";
  if (token == "mf" || token == "m/f" || token == "f/m") return "masc./fem.";
  if (token == "n" && line.size() <= 2) return "noun";  // Spanish packs rarely use n=neuter
  if (token == "v") return "verb";
  if (token == "adj") return "adjective";
  if (token == "adv") return "adverb";
  if (token == "prep") return "preposition";
  if (token == "conj") return "conjunction";
  if (token == "pron") return "pronoun";
  if (token == "interj") return "interjection";
  if (token == "art") return "article";
  if (token == "num") return "numeral";
  if (token == "pl") return "plural";
  if (token == "sg") return "singular";
  if (token == "misc") return "other";
  // Already long-form POS — keep as written (original casing cleaned to lower token only).
  if (token == "noun" || token == "verb" || token == "adjective" || token == "adverb" || token == "pronoun" ||
      token == "preposition" || token == "conjunction" || token == "interjection" || token == "article" ||
      token == "feminine" || token == "masculine" || token == "neuter" || token == "phrase" || token == "idiom") {
    return token;
  }
  return line;
}

// True for any grammar header line (POS or gender) that must not become a numbered sense.
bool isGrammarHeader(const std::string& line) { return isGenderLabel(line) || isPosLabel(line); }

void stripStarDictMarkup(std::string& text) {
  // @noun → noun (POS). @English (pack name from multi-dict cascade) keeps the '@'
  // so the renderer can bold section headers.
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '@' && (i == 0 || text[i - 1] == '\n' || text[i - 1] == '\0')) {
      size_t j = i + 1;
      while (j < text.size() && text[j] != '\n' && text[j] != '\0') j++;
      if (j > i + 1) {
        std::string label(text, i + 1, j - i - 1);
        while (!label.empty() && label.back() == ' ') label.pop_back();
        if (isPosLabel(label)) {
          out.append(label);
        } else {
          // Multi-dict pack header or other section — keep '@' marker.
          out.push_back('@');
          out.append(label);
        }
        i = j;
        continue;
      }
    }
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0x7F || (c < 0x20 && c != '\n' && c != '\t')) {
      if (c == '\t') out.push_back(' ');
      i++;
      continue;
    }
    out.push_back(text[i++]);
  }
  text.swap(out);
  replaceFancyUnicode(text);
  stripNonPrintable(text);
}

// Cap sense count and number them for e-reader readability.
// Input is already newline-separated (POS/gender labels + bare sense lines).
// Output example:
//   verb
//     1. to help
//   ~feminine          ← gender: italic marker, never numbered
//     1. help, aid
//   ~masculine
//     1. helper
// Multi-dict pack headers (@English) are preserved; sense budget is per pack.
// Sense numbers restart under each grammar header and are indented for scanability.
constexpr int kMaxSensesTotal = 3;
constexpr int kMaxSensesPerPack = 3;
constexpr const char* kSenseIndent = "  ";  // two spaces before "1. …"

bool looksLikePronLine(const std::string& line) {
  if (line.size() < 3) return false;
  // /ipa/, [enPR], (respelling) — common StarDict / bilingual pack forms.
  if (line.front() == '/' && line.back() == '/') return true;
  if (line.front() == '[' && line.back() == ']') return true;
  if (line.front() == '(' && line.back() == ')') {
    // Avoid treating full sense sentences in parens as pronunciation.
    if (line.size() > 48) return false;
    // Must contain a vowel-ish letter (not pure "(pl.)" grammar — those are short).
    int letters = 0;
    for (unsigned char c : line) {
      if (std::isalpha(c)) letters++;
    }
    return letters >= 2;
  }
  return false;
}

bool looksLikeNumberedSense(const std::string& line) {
  if (line.size() < 2 || !std::isdigit(static_cast<unsigned char>(line[0]))) return false;
  size_t i = 1;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) i++;
  return i < line.size() && (line[i] == '.' || line[i] == ')' || line[i] == ' ');
}

// Strip existing "1. " / "1) " prefix so we can renumber cleanly.
std::string stripSenseNumber(const std::string& line) {
  if (!looksLikeNumberedSense(line)) return line;
  size_t i = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) i++;
  if (i < line.size() && (line[i] == '.' || line[i] == ')')) i++;
  while (i < line.size() && line[i] == ' ') i++;
  return line.substr(i);
}

// Format one pack body (no @ header): grammar headers + indented numbered senses.
// Clean packs already order senses (verb-first past forms); keep display simple.
std::string formatPackBody(const std::string& body, const int maxSenses) {
  std::vector<std::string> rawLines;
  {
    size_t start = 0;
    while (start <= body.size()) {
      size_t nl = body.find('\n', start);
      if (nl == std::string::npos) {
        rawLines.push_back(body.substr(start));
        break;
      }
      rawLines.push_back(body.substr(start, nl - start));
      start = nl + 1;
    }
  }

  struct Item {
    uint8_t kind;  // 0 pre, 1 pos, 2 gender, 3 sense
    std::string text;
  };
  std::vector<Item> items;
  items.reserve(rawLines.size());

  for (std::string line : rawLines) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
    size_t a = 0;
    while (a < line.size() && line[a] == ' ') a++;
    if (a > 0) line = line.substr(a);
    if (line.empty()) continue;
    if (looksLikePronLine(line)) continue;

    if (!line.empty() && line[0] == '!') {
      items.push_back({0, line.substr(1)});
      continue;
    }

    {
      const std::string maybeGender = stripSenseNumber(line);
      if (isGenderLabel(maybeGender) && (looksLikeNumberedSense(line) || isGenderLabel(line))) {
        items.push_back({2, expandGrammarLabel(maybeGender)});
        continue;
      }
    }
    if (isGenderLabel(line)) {
      items.push_back({2, expandGrammarLabel(line)});
      continue;
    }
    if (isPosLabel(line)) {
      items.push_back({1, expandGrammarLabel(line)});
      continue;
    }
    const std::string core = stripSenseNumber(line);
    if (core.empty() || isGrammarHeader(core) || looksLikePronLine(core)) continue;
    items.push_back({3, core});
  }

  std::string out;
  out.reserve(body.size() + 32);
  int senseCount = 0;
  int senseInSection = 0;

  auto appendLine = [&](const std::string& s) {
    if (!out.empty()) out.push_back('\n');
    out.append(s);
  };

  for (const auto& it : items) {
    if (it.kind == 0) {
      if (!it.text.empty()) appendLine(it.text);
      continue;
    }
    if (it.kind == 1) {
      if (senseCount >= maxSenses && senseInSection > 0) break;
      appendLine(it.text);
      senseInSection = 0;
      continue;
    }
    if (it.kind == 2) {
      if (senseCount >= maxSenses && senseInSection > 0) break;
      appendLine(std::string("~") + it.text);
      senseInSection = 0;
      continue;
    }
    if (senseCount >= maxSenses) break;
    senseCount++;
    senseInSection++;
    char num[24];
    snprintf(num, sizeof(num), "%s%d. ", kSenseIndent, senseInSection);
    appendLine(std::string(num) + it.text);
  }

  return out;
}

// Full definition: optional multi-pack (@Name) sections, each sense-capped.
std::string formatDefinitionForReader(const std::string& text) {
  if (text.empty()) return text;

  // Split on lines that start with '@' (pack headers from multi-dict cascade).
  std::vector<std::pair<std::string, std::string>> packs;  // name (may be empty), body
  {
    size_t start = 0;
    std::string curName;
    std::string curBody;
    auto flush = [&]() {
      if (!curName.empty() || !curBody.empty()) {
        packs.emplace_back(curName, curBody);
      }
      curName.clear();
      curBody.clear();
    };
    while (start <= text.size()) {
      size_t nl = text.find('\n', start);
      std::string line = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
      if (!line.empty() && line[0] == '@') {
        flush();
        curName = line;  // includes '@'
      } else {
        if (!curBody.empty()) curBody.push_back('\n');
        curBody.append(line);
      }
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
    flush();
  }

  if (packs.empty()) {
    return formatPackBody(text, kMaxSensesTotal);
  }

  const int perPack = packs.size() > 1 ? kMaxSensesPerPack : kMaxSensesTotal;
  // When multiple packs, still hard-cap overall senses across all packs.
  int remaining = kMaxSensesTotal;
  std::string out;
  for (const auto& pk : packs) {
    if (remaining <= 0) break;
    const int budget = std::min(perPack, remaining);
    std::string body = formatPackBody(pk.second, budget);
    // Count senses we actually emitted
    int used = 0;
    size_t pos = 0;
    while (pos < body.size()) {
      size_t nl = body.find('\n', pos);
      std::string ln = (nl == std::string::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
      if (looksLikeNumberedSense(ln)) used++;
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    remaining -= used;
    if (!out.empty()) out += "\n\n";
    if (!pk.first.empty()) {
      out += pk.first;
      out += '\n';
    }
    out += body;
  }
  return out;
}

// Extract a pronunciation token into outPron (keeps outer delimiters) and
// remove it from the definition body. Supports /ipa/, [enPR], (respelling).
// Multi-dict bodies often start with "@English\n(mn'stuh)\n..." — skip pack
// headers and scan early content lines.
constexpr size_t kMaxPronBytes = 120;

bool isPronOpenChar(char c) { return c == '/' || c == '[' || c == '('; }
char matchingClose(char open) {
  if (open == '/') return '/';
  if (open == '[') return ']';
  return ')';
}

void extractPronunciation(std::string& text, std::string& outPron) {
  outPron.clear();
  if (text.empty()) return;

  // Walk early lines (skip blank / @pack headers) looking for a pron token.
  size_t lineStart = 0;
  int linesScanned = 0;
  while (lineStart < text.size() && linesScanned < 6 && outPron.empty()) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    // Trim line edges
    size_t a = lineStart;
    size_t b = lineEnd;
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;

    const std::string line = text.substr(a, b - a);
    linesScanned++;

    // Skip multi-dict pack headers and empty lines.
    if (line.empty() || line[0] == '@') {
      lineStart = (lineEnd < text.size()) ? lineEnd + 1 : text.size();
      continue;
    }

    // Whole-line pronunciation: /…/ [… ] (…)
    if (looksLikePronLine(line) && line.size() - 2 <= kMaxPronBytes) {
      outPron = line;
      // Erase this line (+ following newline) from text.
      size_t eraseEnd = lineEnd;
      if (eraseEnd < text.size()) eraseEnd++;  // include '\n'
      text.erase(lineStart, eraseEnd - lineStart);
      break;
    }

    // Pron token at start of a longer line: "/foo/ noun" or "(mn'stuh) a creature"
    if (isPronOpenChar(line[0])) {
      const char open = line[0];
      const char close = matchingClose(open);
      const size_t closePos = line.find(close, 1);
      if (closePos != std::string::npos && closePos > 1 && closePos - 1 <= kMaxPronBytes) {
        outPron = line.substr(0, closePos + 1);
        // Remove only the token (+ trailing spaces) from this line in `text`.
        size_t tokStart = a;
        size_t tokEnd = a + closePos + 1;
        while (tokEnd < lineEnd && (text[tokEnd] == ' ' || text[tokEnd] == '\t')) tokEnd++;
        text.erase(tokStart, tokEnd - tokStart);
        break;
      }
    }

    // First non-header content line without a pron token — stop scanning so we
    // don't yank a parenthetical from deep in a sense.
    break;
  }

  // Trim leftover leading whitespace / blank lines.
  size_t j = 0;
  while (j < text.size() && (text[j] == ' ' || text[j] == '\n' || text[j] == '\t' || text[j] == '\r')) j++;
  if (j > 0) text.erase(0, j);
}

void drawDefinitionScrollBar(GfxRenderer& renderer, const int popupX, const int popupW, const int bodyTop,
                             const int bodyH, const int totalLines, const int visible, const int scroll) {
  if (totalLines <= visible || bodyH <= 4) return;
  const int trackX = popupX + popupW - 8;
  const int trackH = bodyH;
  const int thumbH = std::max(8, trackH * visible / totalLines);
  const int maxScroll = std::max(1, totalLines - visible);
  const int thumbY = bodyTop + (trackH - thumbH) * scroll / maxScroll;
  renderer.drawLine(trackX, bodyTop, trackX, bodyTop + trackH - 1, true);
  renderer.fillRect(trackX - 1, thumbY, 3, thumbH, true);
}

}  // namespace

void DictionaryDefinitionActivity::normalizeDefinition() {
  // StarDict multi-type separators are embedded NULs.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  stripLeadingTypeCodes(definition);
  definition = htmlToPlainText(definition);
  stripStarDictMarkup(definition);
  extractPronunciation(definition, pronunciation);

  // Collapse runs of blank lines to a single blank.
  std::string cleaned;
  cleaned.reserve(definition.size());
  int blankRun = 0;
  for (size_t i = 0; i < definition.size(); i++) {
    if (definition[i] == '\n') {
      if (blankRun < 1) cleaned.push_back('\n');
      blankRun++;
    } else if (definition[i] == '\r') {
      continue;
    } else {
      blankRun = 0;
      cleaned.push_back(definition[i]);
    }
  }
  while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '\n')) cleaned.pop_back();

  // E-reader layout: POS headers + numbered senses (order comes from the pack).
  definition = formatDefinitionForReader(cleaned);
}

void DictionaryDefinitionActivity::layoutPopup() {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  popupW = std::max(200, pageW - kPopupMarginX * 2);
  popupX = (pageW - popupW) / 2;

  // Pronunciation sits on the same line as the bold headword (regular weight),
  // so chrome is always one title line — more room for definitions.
  const int titleLineH = std::max(1, renderer.getLineHeight(UI_12_FONT_ID));
  const int lineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int chromeH = kPopupPadY + titleLineH + kHeaderLineGap + 2 + kTitleBodyGap + kPopupPadY;
  constexpr int minBodyLines = 3;
  const int maxPopupH = std::max(chromeH + lineH * 4, pageH - 80);
  const int contentLines = std::max(minBodyLines, static_cast<int>(lines.size()));
  const int desiredH = chromeH + contentLines * lineH;
  popupH = std::clamp(desiredH, chromeH + minBodyLines * lineH, maxPopupH);
  popupY = std::max(16, (pageH - popupH) / 2);

  bodyTop = popupY + kPopupPadY + titleLineH + kHeaderLineGap + 2 + kTitleBodyGap;
  bodyH = popupY + popupH - kPopupPadY - bodyTop;
  visibleLines = std::max(1, bodyH / lineH);
  if (scrollLine > 0 && scrollLine + visibleLines > static_cast<int>(lines.size())) {
    scrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  }
}

void DictionaryDefinitionActivity::rebuildLines() {
  lines.clear();
  layoutPopup();  // need popupW for wrap width
  const int contentW = std::max(40, popupW - kPopupPadX * 2 - 6);

  if (definition.empty()) {
    lines.emplace_back(tr(STR_DICT_NOT_FOUND));
    return;
  }

  const char* p = definition.c_str();
  while (*p && static_cast<int>(lines.size()) < kMaxBodyLines) {
    const char* nl = strchr(p, '\n');
    std::string para;
    if (nl != nullptr) {
      para.assign(p, static_cast<size_t>(nl - p));
      p = nl + 1;
    } else {
      para = p;
      p += para.size();
    }
    while (!para.empty() && (para.back() == ' ' || para.back() == '\r')) para.pop_back();
    if (para.empty()) {
      if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
      continue;
    }

    const int room = kMaxBodyLines - static_cast<int>(lines.size());
    if (room <= 0) break;

    // Multi-dict section header from cascade: "@English" stays marked for bold draw.
    if (!para.empty() && para[0] == '@') {
      lines.push_back(para);
      continue;
    }

    // Gender (~feminine) and POS (verb) stay one short label line.
    if ((!para.empty() && para[0] == '~') || isGenderLabel(para) || isPosLabel(para)) {
      lines.push_back(para);
      continue;
    }

    // Sense lines are already indented ("  1. …"); wrap continuations further in.
    const bool sense = looksLikeNumberedSense(para) ||
                       (para.size() > 2 && para[0] == ' ' && looksLikeNumberedSense(para.substr(2)));
    const int wrapW = sense ? std::max(40, contentW - 12) : contentW;
    auto wrapped = renderer.wrappedText(UI_10_FONT_ID, para.c_str(), wrapW, room);
    for (size_t i = 0; i < wrapped.size(); ++i) {
      if (static_cast<int>(lines.size()) >= kMaxBodyLines) break;
      if (sense && i > 0) {
        lines.push_back(std::string("     ") + wrapped[i]);  // hang under "  N. "
      } else {
        lines.push_back(std::move(wrapped[i]));
      }
    }
  }
}

bool DictionaryDefinitionActivity::captureBackground() {
  // Snapshot only the card bounds (+ small pad for border), not the full frame
  // (~10–20 KB vs ~48 KB). Outside the card the previous activity's frame remains.
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  if (pageW <= 0 || pageH <= 0 || popupW <= 0 || popupH <= 0) {
    return false;
  }
  constexpr int kPad = 4;
  bgX = std::max(0, popupX - kPad);
  bgY = std::max(0, popupY - kPad);
  bgW = std::min(pageW - bgX, popupW + kPad * 2);
  bgH = std::min(pageH - bgY, popupH + kPad * 2);
  if (bgW <= 0 || bgH <= 0) {
    return false;
  }
  const size_t needed = renderer.getRegionByteSize(bgX, bgY, bgW, bgH);
  if (needed == 0) {
    return false;
  }
  backgroundBuffer = makeUniqueNoThrow<uint8_t[]>(needed);
  if (!backgroundBuffer) {
    LOG_ERR("DICT", "OOM: background snapshot %u bytes", static_cast<unsigned>(needed));
    backgroundBufferSize = 0;
    return false;
  }
  backgroundBufferSize = needed;
  if (!renderer.copyRegionToBuffer(bgX, bgY, bgW, bgH, backgroundBuffer.get(), backgroundBufferSize)) {
    backgroundBuffer.reset();
    backgroundBufferSize = 0;
    return false;
  }
  LOG_DBG("DICT", "Def popup snapshot %dx%d → %u bytes", bgW, bgH, static_cast<unsigned>(needed));
  return true;
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  normalizeDefinition();
  scrollLine = 0;
  // Layout first so captureBackground can size the card region (not full screen).
  popupW = std::max(200, renderer.getScreenWidth() - kPopupMarginX * 2);
  rebuildLines();
  layoutPopup();
  hasBackground = captureBackground();
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  backgroundBuffer.reset();
  backgroundBufferSize = 0;
  hasBackground = false;
  Activity::onExit();
}

void DictionaryDefinitionActivity::loop() {
  // Back → stay in word-select (look up another word). Done / tap → leave dictionary mode.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;  // parent keeps DictionaryWordSelect open
    setResult(std::move(r));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult r;
    r.isCancelled = false;  // parent finishes word-select → back to reading
    setResult(std::move(r));
    finish();
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    ActivityResult r;
    r.isCancelled = false;
    setResult(std::move(r));
    finish();
    return;
  }

  buttonNavigator.onNext([this] {
    if (scrollLine + visibleLines < static_cast<int>(lines.size())) {
      scrollLine++;
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    if (scrollLine > 0) {
      scrollLine--;
      requestUpdate();
    }
  });
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  // Restore page pixels under the card; leave the rest of the frame alone.
  // Fall back to full clear only on OOM/snapshot miss so the card is still readable.
  if (hasBackground && backgroundBuffer) {
    renderer.copyBufferToRegion(bgX, bgY, bgW, bgH, backgroundBuffer.get(), backgroundBufferSize);
  } else {
    renderer.clearScreen();
  }
  layoutPopup();

  // Opaque card: solid white fill first so page text never shows through the
  // rounded edge (1-bit AA corners can leave gaps if we only fillRoundedRect).
  renderer.fillRect(popupX, popupY, popupW, popupH, false);
  renderer.fillRoundedRect(popupX, popupY, popupW, popupH, kCorner, Color::White);
  renderer.fillRoundedRect(popupX + 1, popupY + 1, std::max(1, popupW - 2), std::max(1, popupH - 2),
                           std::max(0, kCorner - 1), Color::White);
  renderer.drawRoundedRect(popupX, popupY, popupW, popupH, 2, kCorner, true);

  // Force UI fonts only (no SD document fonts for dict chrome).
  const int headFont = UI_12_FONT_ID;
  const int bodyFont = UI_10_FONT_ID;

  // Header: bold headword + regular pronunciation on one line, e.g.
  //   pendulous /pĕn′jə-ləs, pĕn′dyə-, -də-/
  // Saves a definition line and matches the common dictionary layout.
  const int contentInnerW = popupW - kPopupPadX * 2;
  const int titleY = popupY + kPopupPadY;
  const int textLeft = popupX + kPopupPadX;
  constexpr int kHeadPronGap = 6;

  std::string pronDisplay = pronunciation;
  if (!pronDisplay.empty()) {
    // Keep original delimiters (/…/ [… ] (…)); only normalize missing-glyph punctuation.
    replaceFancyUnicode(pronDisplay);
    const char c0 = pronDisplay.front();
    if (c0 != '/' && c0 != '[' && c0 != '(') {
      pronDisplay = "/" + pronDisplay + "/";
    }
  }

  // Reserve space for pronunciation first so the bold word truncates if needed.
  int pronW = 0;
  if (!pronDisplay.empty()) {
    pronW = renderer.getTextWidth(bodyFont, pronDisplay.c_str(), EpdFontFamily::REGULAR);
  }
  const int headMaxW =
      !pronDisplay.empty()
          ? std::max(48, contentInnerW - kHeadPronGap - std::min(pronW, contentInnerW / 2 + 20))
          : contentInnerW;
  const std::string titleVis =
      renderer.truncatedText(headFont, headword.c_str(), headMaxW, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(headFont, titleVis.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(headFont, textLeft, titleY, titleVis.c_str(), true, EpdFontFamily::BOLD);

  if (!pronDisplay.empty()) {
    const int remainW = contentInnerW - titleW - kHeadPronGap;
    if (remainW >= 24) {
      const std::string pronVis =
          renderer.truncatedText(bodyFont, pronDisplay.c_str(), remainW, EpdFontFamily::REGULAR);
      // Same baseline as headword; regular weight (not bold).
      const int pronY = titleY + std::max(0, (renderer.getLineHeight(headFont) - renderer.getLineHeight(bodyFont)) / 2);
      renderer.drawText(bodyFont, textLeft + titleW + kHeadPronGap, pronY, pronVis.c_str(), true,
                        EpdFontFamily::REGULAR);
    }
  }

  const int ruleY = titleY + renderer.getLineHeight(headFont) + kHeaderLineGap;
  renderer.drawLine(popupX + kPopupPadX, ruleY, popupX + popupW - kPopupPadX - 1, ruleY, true);

  const int lineH = renderer.getLineHeight(bodyFont);
  bodyTop = ruleY + kTitleBodyGap;
  bodyH = popupY + popupH - kPopupPadY - bodyTop;
  visibleLines = std::max(1, bodyH / std::max(1, lineH));
  if (scrollLine > 0 && scrollLine + visibleLines > static_cast<int>(lines.size())) {
    scrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  }

  int y = bodyTop;
  const int textX = popupX + kPopupPadX;
  const int bodyBottom = popupY + popupH - kPopupPadY;
  for (int i = scrollLine; i < static_cast<int>(lines.size()) && y + lineH <= bodyBottom + 2; ++i) {
    const std::string& line = lines[static_cast<size_t>(i)];
    if (line.empty()) {
      y += lineH / 2;
      continue;
    }
    // Hierarchy: pack headers centered bold, gender italic, POS bold, senses indented.
    if (!line.empty() && line[0] == '@') {
      const char* packName = line.c_str() + 1;
      const int packW = renderer.getTextWidth(bodyFont, packName, EpdFontFamily::BOLD);
      const int packX = popupX + std::max(kPopupPadX, (popupW - packW) / 2);
      renderer.drawText(bodyFont, packX, y, packName, true, EpdFontFamily::BOLD);
    } else if (!line.empty() && line[0] == '~') {
      // Gender label above its senses. UI fonts may lack a true italic face — ITALIC
      // falls back to regular (not bold), which still separates them from POS headers.
      renderer.drawText(bodyFont, textX, y, line.c_str() + 1, true, EpdFontFamily::ITALIC);
    } else if (isGenderLabel(line)) {
      renderer.drawText(bodyFont, textX, y, line.c_str(), true, EpdFontFamily::ITALIC);
    } else if (isPosLabel(line)) {
      renderer.drawText(bodyFont, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(bodyFont, textX, y, line.c_str(), true, EpdFontFamily::REGULAR);
    }
    y += lineH;
  }

  const int totalLines = static_cast<int>(lines.size());
  if (totalLines > visibleLines) {
    drawDefinitionScrollBar(renderer, popupX, popupW, bodyTop, bodyH, totalLines, visibleLines, scrollLine);
  }

  // Wipe any prior activity's button chrome from the restored snapshot so we
  // never stack two sets of Left/Right (or Back/Select) under Back/Done.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int stripH = metrics.buttonHintsHeight + 4;
    const int stripY = renderer.getScreenHeight() - stripH;
    if (stripH > 0 && stripY >= 0) {
      renderer.fillRect(0, stripY, renderer.getScreenWidth(), stripH, false);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE),
                                            (scrollLine > 0 ? tr(STR_DIR_UP) : ""),
                                            (scrollLine + visibleLines < totalLines ? tr(STR_DIR_DOWN) : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
