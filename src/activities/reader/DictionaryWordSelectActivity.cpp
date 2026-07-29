#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

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
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  startMarkIdx = -1;
  multiSelectArmedThisHold = false;
  confirmDown = false;
  // Residual long-press Menu that opened dictionary: wait for Select release.
  ignoreConfirmUntilReleased = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  extractWords();
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Do not select words that sit under the front-button hint strip. The reader
  // page was laid out for status-bar chrome, not the taller dictionary hints;
  // without this limit, last-line tokens highlight behind Back/Lookup buttons.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int maxWordBottom =
      renderer.getScreenHeight() - metrics.buttonHintsHeight - kHintWordClearance;
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

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
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
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
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
    // Cap phrase length for lookup key size.
    if (hi - lo + 1 > kMaxPhraseWords) {
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
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                          std::move(combined)),
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
  if (confirmDown && selectDown && !multiSelectArmedThisHold && startMarkIdx < 0 && !words.empty() &&
      (millis() - confirmDownAtMs) >= MULTI_SELECT_HOLD_MS) {
    // Long-press Select: arm multi-word range at cursor (lookup on a later short Select).
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
    if (!words.empty()) {
      performLookup();
    }
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
  // repaint), a tap on a word selects and looks it up in one go.
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
      performLookup();
    }
    return;
  }

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

void DictionaryWordSelectActivity::drawSelectionHighlights() {
  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  for (int i = lo; i <= hi; ++i) {
    const WordBox& word = words[static_cast<size_t>(i)];
    int hx = word.x - 2;
    int hy = word.y - 2;
    int hw = word.width + 4;
    int hh = lineHeight + 4;
    if (hx < 0) {
      hw += hx;
      hx = 0;
    }
    if (hy < 0) {
      hh += hy;
      hy = 0;
    }
    if (hw > 0 && hh > 0) {
      renderer.fillRect(hx, hy, hw, hh, true);
      renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
    }
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
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
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
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Reader top chrome (battery/clock) is gone in this activity — use that band
// for a quiet mode title so users know they left the reader.
void DictionaryWordSelectActivity::drawModeTitle() const {
  const char* title =
      (startMarkIdx >= 0) ? tr(STR_MULTI_WORD_SELECTION) : tr(STR_DICTIONARY_LOOKUP);
  // UI_10 bold: a step up from SMALL_FONT so the mode label is easy to spot
  // without colliding with body text (page still has top chrome headroom).
  constexpr int kTitleFont = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(kTitleFont);
  const int titleY = std::max(2, (marginTop - lineH) / 2);
  // Light wipe so page ink under the title band does not show through.
  renderer.fillRect(0, 0, renderer.getScreenWidth(), std::max(lineH + titleY + 2, marginTop - 2), false);
  renderer.drawCenteredText(kTitleFont, titleY, title, true, EpdFontFamily::BOLD);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path (single-word only): restore old highlight pixels,
  // paint the new cursor, skip full page re-render.
  if (popup == Popup::None && startMarkIdx < 0 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawModeTitle();
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawModeTitle();
  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
