#include "DictionaryLookupActivity.h"

#include <DictionaryLookup.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxDefBuf = DictionaryLookup::kMaxDefinitionLen + 1;
constexpr int kPopupMarginX = 28;
constexpr int kPopupPadX = 14;
constexpr int kPopupPadY = 12;
constexpr int kCorner = 8;
constexpr int kTitleBodyGap = 8;
constexpr int kHeaderLineGap = 6;
// Match chapter-select / Lyra list scroll chrome. Bar sits in the card's right padding.
constexpr int kScrollBarW = 4;
constexpr int kScrollBarRightOffset = 5;

void drawDefinitionScrollBar(const GfxRenderer& renderer, const int popupLeft, const int popupW, const int bodyTop,
                             const int bodyH, const int totalLines, const int visibleLines, const int scrollLine) {
  if (totalLines <= visibleLines || bodyH < 12 || visibleLines <= 0) {
    return;
  }
  const int scrollBarHeight = std::max(kScrollBarW, (bodyH * visibleLines) / totalLines);
  const int maxStart = std::max(1, totalLines - visibleLines);
  const int clamped = std::clamp(scrollLine, 0, maxStart);
  const int scrollBarY = bodyTop + ((bodyH - scrollBarHeight) * clamped) / maxStart;
  const int scrollBarX = popupLeft + popupW - kScrollBarRightOffset;
  renderer.drawLine(scrollBarX, bodyTop, scrollBarX, bodyTop + bodyH - 1, true);
  renderer.fillRect(scrollBarX - kScrollBarW, scrollBarY, kScrollBarW, scrollBarHeight, true);
}
}

DictionaryLookupActivity::DictionaryLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string lookupWordIn)
    : Activity("DictionaryLookup", renderer, mappedInput), lookupWord(std::move(lookupWordIn)) {}

void DictionaryLookupActivity::onEnter() {
  Activity::onEnter();
  scrollLine = 0;
  ignoreConfirmUntilRelease = true;
  // Open packs once for this popup; close in onExit so SD is free for reading.
  DictionaryLookup::beginSession();
  missingFile = !DictionaryLookup::anyAvailable();
  found = false;
  definition.clear();
  langLabel[0] = '\0';

  if (!missingFile) {
    char buf[kMaxDefBuf];
    buf[0] = '\0';
    found = DictionaryLookup::lookupAuto(lookupWord.c_str(), buf, sizeof(buf), langLabel, sizeof(langLabel));
    if (found) {
      definition = buf;
    }
  }
  layoutPopup();
  rebuildLines();
  requestUpdate();
}

void DictionaryLookupActivity::onExit() {
  DictionaryLookup::endSession();
  Activity::onExit();
}

void DictionaryLookupActivity::layoutPopup() {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  popupW = std::max(200, pageW - kPopupMarginX * 2);
  popupX = (pageW - popupW) / 2;

  const int titleLineH = std::max(1, renderer.getLineHeight(UI_12_FONT_ID));
  const int lineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int chromeH = kPopupPadY + titleLineH + kHeaderLineGap + 2 + kTitleBodyGap + kPopupPadY;
  // Grow with wrapped line count; scroll only if content hits the screen cap.
  const int minBodyLines = 3;
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

void DictionaryLookupActivity::rebuildLines() {
  lines.clear();
  // Full pad-to-pad text column; scrollbar is drawn in the right pad only.
  const int contentW = std::max(40, popupW - kPopupPadX * 2);
  if (missingFile) {
    lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_DICT_NOT_INSTALLED), contentW, 16);
    return;
  }
  if (!found || definition.empty()) {
    char msg[96];
    snprintf(msg, sizeof(msg), tr(STR_DICT_NOT_FOUND_FORMAT), lookupWord.c_str());
    lines = renderer.wrappedText(UI_10_FONT_ID, msg, contentW, 10);
    return;
  }
  // Same line-oriented layout as DictionarySelectionActivity (pack headers + senses).
  constexpr int kMaxBodyLines = 48;
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
    if (!para.empty() && para.back() == '\r') para.pop_back();
    if (para.empty()) {
      if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
      continue;
    }
    if (para[0] == '@') {
      lines.push_back(std::move(para));
      continue;
    }
    const int room = kMaxBodyLines - static_cast<int>(lines.size());
    if (room <= 0) break;
    const bool isSense = para.size() >= 4 && static_cast<unsigned char>(para[0]) == 0xE2 &&
                         static_cast<unsigned char>(para[1]) == 0x80 && static_cast<unsigned char>(para[2]) == 0xA2;
    auto wrapped = renderer.wrappedText(UI_10_FONT_ID, para.c_str(), contentW, room);
    for (size_t i = 0; i < wrapped.size(); ++i) {
      if (static_cast<int>(lines.size()) >= kMaxBodyLines) break;
      if (isSense && i > 0) {
        lines.push_back(std::string("  ") + wrapped[i]);
      } else {
        lines.push_back(std::move(wrapped[i]));
      }
    }
  }
}

void DictionaryLookupActivity::loop() {
  // Swallow the Confirm release from the long-press that opened this screen.
  if (ignoreConfirmUntilRelease) {
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    ignoreConfirmUntilRelease = false;
    // Fall through so Back still works immediately after release.
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
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

void DictionaryLookupActivity::render(RenderLock&&) {
  // Dim/clear background so the card reads as a modal popup.
  renderer.clearScreen();

  layoutPopup();

  // White card with black border (double stroke for e-ink clarity).
  renderer.fillRoundedRect(popupX, popupY, popupW, popupH, kCorner, Color::White);
  renderer.drawRoundedRect(popupX, popupY, popupW, popupH, 2, kCorner, true);

  // Header: looked-up word (+ pack label if known), well above the rule.
  char title[96];
  if (found && langLabel[0] != '\0') {
    snprintf(title, sizeof(title), "%s  [%s]", lookupWord.c_str(), langLabel);
  } else {
    snprintf(title, sizeof(title), "%s", lookupWord.c_str());
  }
  const std::string titleVis =
      renderer.truncatedText(UI_12_FONT_ID, title, popupW - kPopupPadX * 2, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(UI_12_FONT_ID, titleVis.c_str(), EpdFontFamily::BOLD);
  const int titleY = popupY + kPopupPadY;
  renderer.drawText(UI_12_FONT_ID, popupX + (popupW - titleW) / 2, titleY, titleVis.c_str(), true, EpdFontFamily::BOLD);

  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int ruleY = titleY + titleLineH + kHeaderLineGap;
  renderer.drawLine(popupX + kPopupPadX, ruleY, popupX + popupW - kPopupPadX - 1, ruleY, true);

  // Body text
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
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
    // Pack labels: same size as body, bold for section break only.
    if (line[0] == '@') {
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str() + 1, true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true);
    }
    y += lineH;
  }

  const int totalLines = static_cast<int>(lines.size());
  if (totalLines > visibleLines) {
    drawDefinitionScrollBar(renderer, popupX, popupW, bodyTop, bodyH, totalLines, visibleLines, scrollLine);
  }

  // Hints sit under the card (full-width chrome), not inside the overlapping header.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
