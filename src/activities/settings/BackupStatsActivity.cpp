#include "BackupStatsActivity.h"
#include "util/UiGhostPolicy.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/reader/StatsBackup.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BackupStatsActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  backupFileName[0] = '\0';
  requestUpdate();
}

void BackupStatsActivity::onExit() { Activity::onExit(); }

void BackupStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BACKUP_NOW));

  // Keep long confirm / path strings inside the content margins (single-line
  // drawCenteredText clips at the screen edges on narrow X3).
  const int side = std::max(metrics.contentSidePadding, 16);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect body{side, contentTop, pageWidth - side * 2, std::max(40, contentBottom - contentTop)};

  if (state == WARNING) {
    UITheme::drawCenteredWrappedText(renderer, body, UI_10_FONT_ID, tr(STR_BACKUP_STATS_CONFIRM), 6, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    UiGhostPolicy::displayMenuFrame(renderer);
    return;
  }

  if (state == SUCCESS) {
    // Two short lines: status + filename (filename may be long — wrap it).
    const int midY = body.y + body.height / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 24, tr(STR_BACKUP_STATS_DONE), true, EpdFontFamily::BOLD);
    const Rect fileRect{body.x, midY - 4, body.width, body.height / 2};
    UITheme::drawCenteredWrappedText(renderer, fileRect, UI_10_FONT_ID,
                                     backupFileName[0] != '\0' ? backupFileName : "-", 4, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    UiGhostPolicy::displayMenuFrame(renderer);
    return;
  }

  const int midY = body.y + body.height / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, midY - 16, tr(STR_BACKUP_STATS_FAILED), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredWrappedText(renderer, Rect{body.x, midY + 4, body.width, body.height / 2}, UI_10_FONT_ID,
                                   tr(STR_CHECK_SERIAL_OUTPUT), 4, true, EpdFontFamily::REGULAR,
                                   UITheme::TextVerticalAlignment::TOP);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  UiGhostPolicy::displayMenuFrame(renderer);
}

void BackupStatsActivity::runBackup() {
  LOG_DBG("BACKUP_STATS", "Creating reading-stats backup");
  state = backupGlobalStats(true, backupFileName, sizeof(backupFileName)) ? SUCCESS : FAILED;
  requestUpdate();
}

void BackupStatsActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      runBackup();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
  }
}
