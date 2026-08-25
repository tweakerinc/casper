#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictLookupLayout.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/UiGhostPolicy.h"

// Tiny pad so a highlight box (+2) clears the hint strip without making the
// reserved chrome taller than dashboard / settings buttons (buttonHintsHeight).
constexpr int kHintWordClearance = 2;

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
// Long-press Select to arm multi-word range (~0.4s, same as reader shortcuts).
constexpr unsigned long MULTI_SELECT_HOLD_MS = 400;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  if (fontId == 0) fontId = SETTINGS.getReaderFontId();
  // Match the page's line pitch (Tight default ~0.9× advanceY). Using bare
  // getLineHeight made size-10 highlights spill into the next line.
  lineHeight = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()));
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  startMarkIdx = -1;
  multiSelectArmedThisHold = false;
  confirmDown = false;
  // Residual long-press Menu that opened dictionary: wait for Select release.
  ignoreConfirmUntilReleased = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Classic path extracts from Page; Rivulet path already has `words` prebuilt.
  if (page) {
    extractWords();
  } else if (!words.empty()) {
    // Measure widths if caller left them zero.
    for (auto& w : words) {
      if (w.width <= 0 && w.text) {
        w.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, w.text, w.style));
      }
    }
  }
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  if (!page) return;
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Do not select words under the front-button hint strip. Orientation-aware:
  // portrait = bottom band; landscape = logical side band (same as drawButtonHints).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);
  const int maxWordBottom = safe.y + safe.height - kHintWordClearance;
  const int minWordLeft = safe.x + kHintWordClearance;
  const int maxWordRight = safe.x + safe.width - kHintWordClearance;
  // lineHeight is set in onEnter() before extractWords().
  const int lh = std::max(1, lineHeight);

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    const int lineY = line->yPos + marginTop;
    // Whole line below the safe band: skip without walking tokens.
    if (lineY + lh > maxWordBottom) {
      continue;
    }

    bool rowHasWords = false;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      const int wordX = line->xPos + block->wordXpos(i) + marginLeft;
      // Landscape: skip tokens that sit under the side hint strip.
      if (wordX >= maxWordRight) continue;

      WordBox box;
      box.x = static_cast<int16_t>(std::max(wordX, minWordLeft));
      box.y = static_cast<int16_t>(lineY);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  const Rect safeClip = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int clipRight = safeClip.x + safeClip.width - kHintWordClearance;
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
    // Clamp width so highlight never crosses into the landscape side chrome.
    if (word.x + word.width > clipRight) {
      word.width = static_cast<int16_t>(std::max(0, clipRight - word.x));
    }
  }
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + lineHeight + SLOP) {
      return i;
    }
  }
  return -1;
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::selectionBounds(int& lo, int& hi) const {
  lo = selected;
  hi = selected;
  if (startMarkIdx >= 0) {
    lo = std::min(startMarkIdx, selected);
    hi = std::max(startMarkIdx, selected);
    // Dictionary: cap phrase length for lookup key size.
    // Clip: allow the full page range (CLIPPING_TEXT_MAX truncates on save).
    if (mode_ == Mode::Dictionary && hi - lo + 1 > kMaxPhraseWords) {
      if (selected >= startMarkIdx) {
        lo = hi - (kMaxPhraseWords - 1);
      } else {
        hi = lo + (kMaxPhraseWords - 1);
      }
    }
  }
  if (lo < 0) lo = 0;
  if (hi >= static_cast<int>(words.size())) hi = static_cast<int>(words.size()) - 1;
}

// Build a lookup token from the selection. Soft-hyphen / end-of-line hyphen
// splits are glued; multi-word ranges join with spaces.
std::string DictionaryWordSelectActivity::buildLookupToken() const {
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) {
    return {};
  }

  auto endsWithBreakHyphen = [](const std::string& s) -> bool {
    if (s.empty()) return false;
    if (s.back() == '-') return true;
    // UTF-8 soft hyphen U+00AD = C2 AD
    return s.size() >= 2 && static_cast<unsigned char>(s[s.size() - 2]) == 0xC2 &&
           static_cast<unsigned char>(s[s.size() - 1]) == 0xAD;
  };
  auto stripTrailingBreakHyphen = [](std::string& s) {
    if (s.empty()) return;
    if (s.back() == '-') {
      s.pop_back();
      return;
    }
    if (s.size() >= 2 && static_cast<unsigned char>(s[s.size() - 2]) == 0xC2 &&
        static_cast<unsigned char>(s[s.size() - 1]) == 0xAD) {
      s.resize(s.size() - 2);
    }
  };

  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);

  std::string phrase;
  for (int i = lo; i <= hi; ++i) {
    std::string token = words[static_cast<size_t>(i)].text ? words[static_cast<size_t>(i)].text : "";
    // Join continuation fragments on the same page (hyphenation / soft-hyphen).
    while (i + 1 <= hi && endsWithBreakHyphen(token)) {
      stripTrailingBreakHyphen(token);
      const char* next = words[static_cast<size_t>(i + 1)].text;
      if (next && *next) token += next;
      ++i;
    }
    if (token.empty()) continue;
    if (!phrase.empty()) phrase += ' ';
    phrase += token;
  }
  return phrase;
}

void DictionaryWordSelectActivity::performLookup() {
  std::vector<std::string> dictNames;
  SETTINGS.getEnabledDictionaries(dictNames);
  if (dictNames.empty()) {
    // Safety net: auto-use every installed pack so EN + bilingual cascade still
    // works if settings were cleared or never multi-selected.
    std::vector<DictionaryEntry> installed;
    DictionaryRegistry::discover(installed);
    dictNames.reserve(installed.size());
    for (const auto& e : installed) {
      dictNames.push_back(e.name);
    }
  }
  if (dictNames.empty()) {
    popup = Popup::Error;
    popupMsg = StrId::STR_DICT_NONE_SELECTED;
    popupTime = millis();
    requestUpdate();
    return;
  }

  popup = Popup::Busy;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();

  // One pass per pack: open → index if needed → lookup. Avoids a second open
  // cycle (resolve + exists probes) before searching. SD still only holds one
  // reader at a time, so packs are sequential.
  //
  // buildLookupToken joins a multi-word range with spaces. Dictionary::lookup
  // expands that into collocation windows + per-token stems (so "Ayudame por
  // favor" can hit "por favor" / "ayudar" even when the full phrase is absent).
  const std::string rawToken = buildLookupToken();
  std::string combined;
  std::string headword;
  bool anyOpened = false;
  bool anyFound = false;
  const bool multi = dictNames.size() > 1;
  bool showedIndexing = false;

  auto runLookup = [&](const char* token) {
    for (const std::string& name : dictNames) {
      Dictionary dict;
      if (!dict.open(name.c_str())) {
        continue;
      }
      anyOpened = true;

      if (dict.needsIndex()) {
        if (!showedIndexing) {
          popupMsg = StrId::STR_DICT_INDEXING;
          requestUpdateAndWait();
          showedIndexing = true;
        }
        (void)dict.buildIndex(&indexBuildYield);
        popupMsg = StrId::STR_DICT_LOOKING_UP;
        requestUpdateAndWait();
      }

      std::string definition;
      std::string matched;
      if (!dict.lookup(token, definition, matched)) {
        continue;
      }
      anyFound = true;
      if (headword.empty()) {
        headword = matched.empty() ? Dictionary::cleanWord(token) : matched;
      }
      if (multi) {
        if (!combined.empty()) combined += "\n\n";
        // '@' → bold pack header in DictionaryDefinitionActivity.
        combined += '@';
        combined += name;
        combined += '\n';
        combined += definition;
      } else {
        if (combined.empty()) {
          combined = std::move(definition);
        } else {
          // Multi-word token fallback may hit several keys in one pack.
          combined += "\n\n";
          combined += definition;
        }
      }
    }
  };

  runLookup(rawToken.c_str());

  // Extra safety for multi-word ranges: if the phrase path still misses, try
  // each word alone and merge hits. Dictionary::lookup already expands windows
  // and stems for a single call; this covers packs/candidates that still miss.
  if (!anyFound) {
    const std::string cleaned = Dictionary::cleanWord(rawToken.c_str());
    if (cleaned.find(' ') != std::string::npos) {
      size_t start = 0;
      while (start < cleaned.size()) {
        while (start < cleaned.size() && cleaned[start] == ' ') ++start;
        if (start >= cleaned.size()) break;
        size_t end = cleaned.find(' ', start);
        if (end == std::string::npos) end = cleaned.size();
        if (end > start) {
          const std::string tok = cleaned.substr(start, end - start);
          runLookup(tok.c_str());
        }
        start = (end < cleaned.size()) ? end + 1 : end;
      }
    }
  }

  if (anyFound) {
    // Clear "Looking up…" and repaint a clean page *before* the definition
    // activity snapshots the framebuffer — otherwise the busy popup and the
    // word-select button chrome get baked into the background (double buttons
    // + stuck "Looking up…" under the definition card).
    popup = Popup::None;
    snapshotIdx = -1;
    requestUpdateAndWait();
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword), std::move(combined)),
        [this](const ActivityResult& result) {
          popup = Popup::None;
          snapshotIdx = -1;
          // Done (or tap): leave dictionary mode entirely → reader.
          // Back: keep word-select so the user can look up another word.
          if (!result.isCancelled) {
            finish();
            return;
          }
          requestUpdate();
        });
    return;
  }
  popup = anyOpened ? Popup::NotFound : Popup::Error;
  popupMsg = anyOpened ? StrId::STR_DICT_NOT_FOUND : StrId::STR_DICT_ERROR;
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::performClipConfirm() {
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // First Select: set range start (classic Clipping Tool).
  if (startMarkIdx < 0) {
    startMarkIdx = selected;
    snapshotIdx = -1;
    requestUpdate();
    return;
  }

  // Second Select: confirm end and return the selected text.
  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  std::string text = buildLookupToken();
  if (text.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  ClippingResult clip;
  clip.text = std::move(text);
  clip.fromWordIdx = lo;
  clip.toWordIdx = hi;
  clip.startPageWordIndex = static_cast<uint16_t>(std::max(0, lo));
  clip.endPageWordIndex = static_cast<uint16_t>(std::max(0, hi));
  clip.wordCount = static_cast<uint16_t>(std::max(0, hi - lo + 1));
  // Page indices filled by the reader (single-page Rivulet harvest).
  clip.sectionPage = 0;
  clip.endSectionPage = 0;
  clip.sectionPageCount = 1;
  if (words[static_cast<size_t>(lo)].text) clip.startText = words[static_cast<size_t>(lo)].text;
  if (words[static_cast<size_t>(hi)].text) clip.endText = words[static_cast<size_t>(hi)].text;
  setResult(ActivityResult{std::move(clip)});
  finish();
}

void DictionaryWordSelectActivity::handleSelectAction() {
  if (words.empty()) return;
  if (mode_ == Mode::Clip) {
    performClipConfirm();
  } else {
    performLookup();
  }
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  using Button = MappedInputManager::Button;
  const bool selectDown = mappedInput.isPressed(Button::Confirm);
  const bool selectReleased = mappedInput.wasReleased(Button::Confirm);

  if (ignoreConfirmUntilReleased) {
    if (!selectDown) {
      ignoreConfirmUntilReleased = false;
      confirmDown = false;
      multiSelectArmedThisHold = false;
    }
    if (mappedInput.wasReleased(Button::Back)) {
      finish();
    }
    return;
  }

  if (selectDown && !confirmDown) {
    confirmDown = true;
    confirmDownAtMs = millis();
    multiSelectArmedThisHold = false;
  }
  // Dictionary only: long-press Select arms multi-word range.
  // Clip mode uses two short Selects (start mark → confirm end), like classic.
  if (mode_ == Mode::Dictionary && confirmDown && selectDown && !multiSelectArmedThisHold && startMarkIdx < 0 &&
      !words.empty() && (millis() - confirmDownAtMs) >= MULTI_SELECT_HOLD_MS) {
    startMarkIdx = selected;
    multiSelectArmedThisHold = true;
    snapshotIdx = -1;  // force full repaint for range highlight
    requestUpdate();
  }

  if (selectReleased) {
    const bool armedMulti = multiSelectArmedThisHold;
    confirmDown = false;
    multiSelectArmedThisHold = false;
    if (armedMulti) {
      // Release after arming range — user extends with Left/Right next.
      return;
    }
    handleSelectAction();
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx >= 0) {
      startMarkIdx = -1;
      confirmDown = false;
      multiSelectArmedThisHold = false;
      snapshotIdx = -1;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (words.empty()) return;

  // Touch: a touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word acts like Select.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      selected = hit;
      handleSelectAction();
    }
    return;
  }

  // Button::Up/Down/Left/Right are logical screen directions (MappedInputManager
  // applies Orient Front Buttons for Portrait 180° / Landscape CCW), matching
  // the captions from mapLabels().
  if (mappedInput.wasPressed(Button::Left) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Right) && selected + 1 < static_cast<int>(words.size())) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(Button::Down)) {
    moveVertical(1);
  }
}

void DictionaryWordSelectActivity::highlightBoxFor(const WordBox& word, int& hx, int& hy, int& hw, int& hh) const {
  // Horizontal pad only. Fill must cover every white drawText pixel or ink
  // outside the black rect is painted white (vanishes — bad in dark mode).
  const int padX = std::max(1, std::min(2, lineHeight / 16));
  hx = word.x - padX;
  hw = word.width + padX * 2;

  // One layout line: compressed pitch, or exact gap to the next row.
  int cellH = lineHeight;
  int nextRowY = INT_MAX;
  for (const auto& w : words) {
    if (w.row == static_cast<uint16_t>(word.row + 1) && w.y < nextRowY) {
      nextRowY = w.y;
    }
  }
  if (nextRowY != INT_MAX && nextRowY > word.y) {
    cellH = nextRowY - word.y;
  }

  // drawText: baseline = word.y + ascender. Bottom of the band needs to clear
  // descenders (g/y/p). Top of the line box is usually empty above Latin caps
  // (ascender covers accents / max extent) — pull the top down so the bar looks
  // centered on the word (still enough black above caps / accents).
  const int asc = std::max(1, renderer.getFontAscenderSize(fontId));
  const int minInk = asc + std::max(2, asc / 6);
  const int bottom = word.y + std::max(cellH, minInk);
  // ~30% of ascender is typical empty headroom above body caps.
  const int topInset = std::max(2, (asc * 3) / 10);
  hy = word.y + topInset;
  hh = bottom - hy;
  if (hh < 6) {
    hy = word.y;
    hh = bottom - hy;
  }

  // Landscape: front-button chrome sits on a logical side. Clamp so the black
  // bar does not run under the hint strip (looked like the menu overlapping).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);
  if (hx < safe.x) {
    hw -= (safe.x - hx);
    hx = safe.x;
  }
  if (hy < safe.y) {
    hh -= (safe.y - hy);
    hy = safe.y;
  }
  const int safeRight = safe.x + safe.width;
  const int safeBottom = safe.y + safe.height;
  if (hx + hw > safeRight) hw = safeRight - hx;
  if (hy + hh > safeBottom) hh = safeBottom - hy;
  if (hw < 0) hw = 0;
  if (hh < 0) hh = 0;
}

void DictionaryWordSelectActivity::paintWordHighlight(const WordBox& word) const {
  if (!word.text) return;
  int hx = 0, hy = 0, hw = 0, hh = 0;
  highlightBoxFor(word, hx, hy, hw, hh);
  if (hw <= 0 || hh <= 0) return;
  // Fill first so every white glyph pixel lands on black (no erased halves).
  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
}

void DictionaryWordSelectActivity::drawSelectionHighlights() {
  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  for (int i = lo; i <= hi; ++i) {
    paintWordHighlight(words[static_cast<size_t>(i)]);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box / multi-word range) — the highlight is drawn
// regardless, but the next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  if (startMarkIdx >= 0) {
    drawSelectionHighlights();
    snapshotIdx = -1;
    return false;
  }

  const WordBox& word = words[selected];
  int hx = 0, hy = 0, hw = 0, hh = 0;
  highlightBoxFor(word, hx, hy, hw, hh);

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  paintWordHighlight(word);
  return saved;
}

// Front-button pills only — no full-width white strip (that blanked the last
// lines of page text). Page layout now reserves bottom chrome so words end
// above this zone; drawButtonHints paints white only inside each pill.
void DictionaryWordSelectActivity::drawHints() const {
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  // previous/next args label UP/DOWN functions after remap. LEFT/RIGHT functions
  // still get hardcoded Left/Right from mapLabels (word-step actions).
  // Passing Left/Right here made remapped Up/Down buttons read "Left"/"Right".
  // Clip mode mirrors classic: Select (mark start) then Done (confirm end).
  const char* confirmLabel = (mode_ == Mode::Clip && startMarkIdx >= 0) ? tr(STR_DONE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Reader top chrome (battery/clock) is gone in this activity — use that band
// for a quiet mode title so users know they left the reader.
void DictionaryWordSelectActivity::drawModeTitle() const {
  const char* title;
  if (mode_ == Mode::Clip) {
    title = (startMarkIdx >= 0) ? tr(STR_MULTI_WORD_SELECTION) : tr(STR_CLIPPING_TOOL);
  } else {
    title = (startMarkIdx >= 0) ? tr(STR_MULTI_WORD_SELECTION) : tr(STR_DICTIONARY_LOOKUP);
  }
  // UI_10 bold: sit in the viewable band (bezel + air), not y=2 inside it.
  constexpr int kTitleFont = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(kTitleFont);
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  (void)oRight;
  (void)oBottom;
  (void)oLeft;
  const int titleY = dictlookup::modeTitleY(oTop);
  renderer.fillRect(0, 0, renderer.getScreenWidth(), dictlookup::modeTitleWipeH(titleY, lineH, marginTop), false);
  renderer.drawCenteredText(kTitleFont, titleY, title, true, EpdFontFamily::BOLD);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path (single-word only): restore old highlight pixels,
  // paint the new cursor, skip full page re-render.
  if (popup == Popup::None && startMarkIdx < 0 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before redrawing white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawModeTitle();
      drawHints();
      ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen(0xFF);

  if (pageFb_ && pageFbBytes_ > 0 && renderer.getFrameBuffer() && pageFbBytes_ <= renderer.getBufferSize()) {
    // Rivulet: restore the page snapshot captured when the tool opened.
    std::memcpy(renderer.getFrameBuffer(), pageFb_.get(), pageFbBytes_);
  } else if (page) {
    // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
    // the in-RAM glyph cache during the real draw.
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    page->render(renderer, fontId, marginLeft, marginTop);
    scope.endScanAndPrewarm();
    page->render(renderer, fontId, marginLeft, marginTop);
  } else if (!words.empty()) {
    // No snapshot (OOM) and no classic Page: redraw selectable tokens as body
    // ink so the tool is still usable instead of a blank plate.
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      std::string warm;
      warm.reserve(words.size() * 8);
      uint8_t styleMask = 0;
      for (const auto& w : words) {
        if (!w.text) continue;
        warm += w.text;
        warm += ' ';
        styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(w.style) & 0x03));
      }
      if (styleMask == 0) styleMask = 0x01;
      fcm->prewarmCache(fontId, warm.c_str(), styleMask);
    }
    for (const auto& w : words) {
      if (!w.text) continue;
      renderer.drawText(fontId, w.x, w.y, w.text, true, w.style);
    }
  }

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawModeTitle();
  drawHints();

  if (popup == Popup::Busy) {
    // Looking up / indexing: small upper-left status (no center pill ghosting).
    snapshotIdx = -1;
    GUI.drawTopLeftStatus(renderer, I18N.get(popupMsg), /*refresh=*/false);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }
  if (popup != Popup::None) {
    // Not found / error: keep the center dialog so the result is hard to miss.
    snapshotIdx = -1;
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  // Dark Mode: invert at display time so page + BW highlight keep polarity.
  ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
}
