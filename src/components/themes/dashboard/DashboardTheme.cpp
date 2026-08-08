#include "DashboardTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
// Dual-device binary (X3+X4): shared Dashboard shell/chrome on both.
// Only RTC-backed stats (streak / calendar dates) branch on gpio.deviceIsX3().

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <numeric>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "fontIds.h"

namespace {
// Dashboard home layout. Card composition is the shipping default for all
// Dashboard theme ids (including legacy Magazine/Card enum values).
// 3 = full-width hero panel aligned with lifetime stats; cover left, stats right inside.
int dashboardLayoutVariant() {
  using T = CrossPointSettings::UI_THEME;
  switch (static_cast<T>(SETTINGS.uiTheme)) {
    case T::STATS_LIFE:
    case T::DASHBOARD_RECENTS:
    case T::DASHBOARD_SCROLL:
    case T::DASHBOARD_CARD:      // legacy separate theme id
    case T::DASHBOARD_MAGAZINE:  // legacy experimental id
      return 3;
    default:
      // Non-dashboard themes do not call this path for home composition.
      return 3;
  }
}

bool isDashboardRecentsTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::DASHBOARD_RECENTS;
}

constexpr int kContentInsetX4 = 20;
constexpr int kContentInsetX3 = 75;
constexpr int kTopInset = 8;
constexpr int kCoverCornerRadius = 8;
constexpr int kStatsColumnWidth = 105;
constexpr int kStatsColumnWidthWide = 120;
constexpr int kCoverStatsGap = 10;
// Card layout: padding inside the hero panel around cover + stats.
// Keep a border but minimize pad so the cover recovers pre-border scale.
constexpr int kHeroCardPad = 6;
// Title sits above cover; keep compact so the hero owns vertical space.
constexpr int kTitleTopPad = 0;
constexpr int kTitleCoverGap = 2;
// Small gap between hero card and Lifetime Stats (home streaks removed).
constexpr int kCoverMetaGap = 6;
// Card layout: padding inside the hero panel (was 6 — tighter so cover can grow).
// Keep a thin left inset so art clears the rounded stroke without wasting width.
constexpr int kHeroCoverLeftInset = 4;
constexpr int kFooterIconSize = 24;
constexpr int kFooterIconTextGap = 8;
// X3: Time, Time Left, Progress, Daily Avg, Pages/Min, Started, Finish
// X4: Time, Time Left, Progress, Pages/Min, Sessions, Avg Session, Pages
// (no calendar rows - no Daily/Started/Finish without RTC).
constexpr int kStatsRowCount = 7;
constexpr int kStatsRowCountX4 = 7;
// Within a stat pair: pull label close under the value (full lineH left a dead band).
constexpr int kStatsValueLabelPull = 3;  // subtract from value lineH before label
// Between pairs: clear air so a label never looks like it belongs to the next value.
constexpr int kStatsInterPairGapMin = 4;
// Pin lifetime to pack bottom (toward menu).
constexpr int kLifetimeStatsBottomPad = 0;
// Lifetime card: compact header + two body rows.
constexpr int kLifetimeTitleH = 16;
constexpr int kLifetimeCellPadY = 1;
// Floor height for the lifetime card when reserving space under the cover.
constexpr int kLifetimeMinCardH = 102;
// Kept for parked drawMetaStatsUnderCover (no longer called on home).
constexpr int kMetaBandH = kFooterIconSize + 4;
// Used by footer helpers that still share a value/label gap constant.
constexpr int kStatsValueLabelGap = 1;

bool isWideScreen(const GfxRenderer& renderer) { return renderer.getScreenWidth() >= 560; }

int contentInset(const GfxRenderer& renderer) { return isWideScreen(renderer) ? kContentInsetX3 : kContentInsetX4; }

// Minimum lifetime card height for layout reserve (header + two value/label rows).
int minLifetimeCardHeight(const GfxRenderer& renderer) {
  const int valueH = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowH = valueH + 1 + labelH + kLifetimeCellPadY * 2;
  // Body bottom inset (see drawLifetimeStatsCard) + a little air so labels clear the border.
  constexpr int kBodyChrome = 1 + 11;  // top + bottom body insets (labels clear stroke)
  return std::max(kLifetimeMinCardH, kLifetimeTitleH + rowH * 2 + kBodyChrome + 2);
}

int minLifeCardHeight(const GfxRenderer& renderer) { return minLifetimeCardHeight(renderer); }

// Height needed for all book-stat value/label pairs (with min inter-pair gaps).
// Hero card must be at least this tall so no stat sits outside the border.
int minBookStatsStackHeight(const GfxRenderer& renderer) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int pairH = std::max(1, valueLineH - kStatsValueLabelPull) + labelLineH;
  const int rowCount = kStatsRowCount;  // worst-case 7 rows (X3 and X4)
  return pairH * rowCount + kStatsInterPairGapMin * (rowCount - 1);
}

// Recents shelf under the hero: up to 4 covers, cover-only (no title band).
constexpr int kRecentsShelfGap = 8;
constexpr int kRecentsShelfBottomPad = 4;
constexpr int kRecentsShelfVisible = 4;

// Forward — contentFrame is defined below; height only needs content width.
int recentsShelfHeight(const GfxRenderer& renderer);

// Space reserved under the cover for meta row + lifetime card (no overlap),
// or the recent-books shelf on Stats Recents.
int lowerDashboardReserve(const GfxRenderer& renderer) {
  if (isDashboardRecentsTheme()) {
    return kCoverMetaGap + recentsShelfHeight(renderer);
  }
  return kCoverMetaGap + minLifetimeCardHeight(renderer) + kLifetimeStatsBottomPad;
}

// Shared horizontal frame for title, cover, right stats, achievements, lifetime card.
// One left edge + one right edge across the whole dashboard.
struct ContentFrame {
  int left = 0;
  int width = 0;
  int right() const { return left + width; }  // exclusive right (x of first pixel past content)
};

ContentFrame contentFrame(const GfxRenderer& renderer) {
  const int inset = contentInset(renderer);
  ContentFrame f;
  f.left = inset;
  f.width = std::max(1, renderer.getScreenWidth() - inset * 2);
  return f;
}

int recentsShelfHeight(const GfxRenderer& renderer) {
  const ContentFrame frame = contentFrame(renderer);
  // Four equal cover slots inside hero-style card padding.
  const int innerW = std::max(1, frame.width - kHeroCardPad * 2);
  const int gaps = kRecentsShelfGap * (kRecentsShelfVisible - 1);
  const int slotW = std::max(36, (innerW - gaps) / kRecentsShelfVisible);
  // ~2:3 book aspect (width:height).
  const int coverH =
      std::max(48, (slotW * DashboardMetrics::homeCoverImageHeight + DashboardMetrics::homeCoverImageWidth / 2) /
                       DashboardMetrics::homeCoverImageWidth);
  // Cap so the hero still has room on short screens.
  const int cap = std::max(70, renderer.getScreenHeight() / 5);
  const int artH = std::min(coverH, cap);
  // Card = pad + covers + pad (+ small pack bottom pad).
  return artH + kHeroCardPad * 2 + kRecentsShelfBottomPad;
}

// Largest single-line title face that fits the content width (bold + embolden).
// Missing faces report width 0 and must be skipped. Layout always reserves the
// tallest face height (see titleBandHeight) so hero/stats/lifetime sizes stay fixed.
int pickSingleLineTitleFont(const GfxRenderer& renderer, const char* title, const int maxWidth) {
  // Prefer biggest readable face; step down only when the full title will not fit.
  static constexpr int kCandidates[] = {
      SOURCESERIF4_18_FONT_ID, SOURCESERIF4_16_FONT_ID, SOURCESERIF4_14_FONT_ID,
      SOURCESERIF4_12_FONT_ID, UI_12_FONT_ID,           UI_10_FONT_ID,
  };
  // Embolden draws a second pass at x+1 — leave 1px so long titles do not clip.
  const int fitW = std::max(1, maxWidth - 1);
  for (const int fontId : kCandidates) {
    const int w = renderer.getTextWidth(fontId, title, EpdFontFamily::BOLD);
    if (w > 0 && w <= fitW) {
      return fontId;
    }
  }
  return UI_10_FONT_ID;
}

// Tallest title face — fixed layout reserve so cover/meta/lifetime never reflow
// when a short title picks 18 vs a long title that falls back to 12.
int maxTitleLineHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(SOURCESERIF4_18_FONT_ID); }

// Centered bold title within a content frame. Reading faces are 2-bit AA and look
// thin on pure-BW home; a 1px horizontal second pass adds stroke weight.
void drawCenteredBoldTitleInFrame(const GfxRenderer& renderer, const ContentFrame& frame, const int fontId, const int y,
                                  const char* text, const bool black) {
  constexpr auto kStyle = EpdFontFamily::BOLD;
  const int textW = renderer.getTextWidth(fontId, text, kStyle);
  // Leave 1px for the embolden pass so long titles do not clip the right edge.
  const int x = frame.left + std::max(0, (frame.width - textW - 1) / 2);
  renderer.drawText(fontId, x, y, text, black, kStyle);
  renderer.drawText(fontId, x + 1, y, text, black, kStyle);
}

// Title band: fixed height (largest face) so the rest of the dashboard pack is stable.
int titleBandHeight(const GfxRenderer& renderer, const RecentBook& /*book*/) {
  return maxTitleLineHeight(renderer) + kTitleTopPad + kTitleCoverGap;
}

// Draws a single-line, bold book title at the top of contentRect. Returns Y just below
// the fixed title band (same for every title length).
int drawTopBookTitle(const GfxRenderer& renderer, const Rect& contentRect, const RecentBook& book,
                     const bool black = true) {
  const ContentFrame frame = contentFrame(renderer);
  // Full frame width for fit; embolden margin is applied inside the picker.
  const int maxTextW = std::max(1, frame.width);
  const char* rawTitle = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int fontId = pickSingleLineTitleFont(renderer, rawTitle, maxTextW);
  const int fitW = std::max(1, maxTextW - 1);
  const std::string line = renderer.truncatedText(fontId, rawTitle, fitW, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(fontId);
  const int bandH = titleBandHeight(renderer, book);
  // Vertically center smaller faces in the fixed band (above the cover gap).
  const int textAreaH = std::max(lineH, bandH - kTitleCoverGap - kTitleTopPad);
  const int titleY = contentRect.y + kTitleTopPad + std::max(0, (textAreaH - lineH) / 2);
  drawCenteredBoldTitleInFrame(renderer, frame, fontId, titleY, line.c_str(), black);
  return contentRect.y + bandH;
}

// Size cover plate to the on-disk Stats hero (1:1 blit). Gen height is chosen so
// width matches the Stats cover column — see HomeCoverMetrics::dashboardHeroThumbHeight.
void sizeCoverFrame(const int maxW, const int maxH, const int pageW, int& coverW, int& coverH) {
  const int capW = std::max(40, maxW);
  const int capH = std::max(80, maxH);
  const int heroH = HomeCoverMetrics::dashboardHeroThumbHeight(pageW, capH);
  const int heroW = HomeCoverMetrics::thumbWidthForHeight(heroH);
  coverW = std::min(capW, heroW);
  coverH = std::min(capH, heroH);
  if (coverH < heroH) {
    coverW = std::min(capW, HomeCoverMetrics::thumbWidthForHeight(coverH));
  }
}

// Forward: measured stats column (defined with drawDashboardStats helpers below).
int bookStatsColumnWidth(const GfxRenderer& renderer);

// Preferred cover size for pack height budget — aspect frame left of stats (not full-bleed).
void preferredCoverSize(const GfxRenderer& renderer, const int maxCoverH, int& coverW, int& coverH) {
  const ContentFrame frame = contentFrame(renderer);
  const int statsW = bookStatsColumnWidth(renderer);
  const int maxCoverW = std::max(80, frame.width - statsW - kCoverStatsGap);
  // Width-first gen plate (matches layoutHeroBlock).
  coverW = maxCoverW;
  coverH = HomeCoverMetrics::thumbHeightForCoverWidth(coverW);
  const int capH = std::max(80, maxCoverH);
  if (coverH > capH) {
    coverH = capH;
    coverW = HomeCoverMetrics::thumbWidthForHeight(coverH);
    if (coverW > maxCoverW) coverW = maxCoverW;
  }
}

// Front-button strip height used for home layout. Theme metrics zero this when
// touch is present (no on-screen hints), but Dashboard home always draws the
// four labels on button devices — never let layout run into that strip.
int homeButtonHintsReserve() {
  if (gpio.hasTouch()) {
    return 0;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Prefer live metrics; fall back to the Lyra/Minimal native strip height.
  return metrics.buttonHintsHeight > 0 ? metrics.buttonHintsHeight : 40;
}

// Free band under the real battery/clock row and above the button strip, with
// breathing room so lifetime stats never sit flush on the labels.
void homeContentBand(const GfxRenderer& renderer, int& bandTop, int& bandBottom) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Compact chrome: battery icon + % / clock sit near the top; do NOT use the
  // full Lyra batteryBarHeight (40) which leaves a large empty band under the clock.
  const int chromeBottom = metrics.topPadding + BaseTheme::kTopChromeBatteryY +
                           std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
  bandTop = chromeBottom + 2;

  // Extra margin above the button labels (was 0 — felt glued to the strip).
  constexpr int kAboveHintsPad = 10;
  const int hintsH = homeButtonHintsReserve();
  bandBottom = renderer.getScreenHeight() - hintsH - kAboveHintsPad;
  if (bandBottom < bandTop + 200) {
    bandBottom = renderer.getScreenHeight() - hintsH - 4;
  }
}

// Measured title→lifetime stack, vertically centered in the free band.
// Pack height equals real content (fixed lifetime height — no stretch-to-fill).
Rect centeredDashboardPackRect(const GfxRenderer& renderer, const Rect& /*contentRect*/, const RecentBook& book) {
  int bandTop = 0;
  int bandBottom = 0;
  homeContentBand(renderer, bandTop, bandBottom);
  const int bandH = std::max(200, bandBottom - bandTop);

  const int titleH = titleBandHeight(renderer, book);
  // Meta+lifetime, or recents shelf under the hero.
  const int belowHero = isDashboardRecentsTheme()
                            ? (kCoverMetaGap + recentsShelfHeight(renderer))
                            : (kCoverMetaGap + minLifetimeCardHeight(renderer) + kLifetimeStatsBottomPad);
  const int heroPad = (dashboardLayoutVariant() == 3) ? (kHeroCardPad * 2) : 0;
  const int fixedH = titleH + heroPad + belowHero;

  int coverBudget = std::max(80, bandH - fixedH);
  int coverW = 0;
  int coverH = 0;
  preferredCoverSize(renderer, coverBudget, coverW, coverH);

  // Card layout: portrait cover left of stats (same aspect as gen thumbs).
  if (dashboardLayoutVariant() == 3) {
    const ContentFrame frame = contentFrame(renderer);
    const int statsW = bookStatsColumnWidth(renderer);
    const int maxCoverW = std::max(40, frame.width - kHeroCardPad * 2 - kCoverStatsGap - statsW);
    coverW = maxCoverW;
    coverH = HomeCoverMetrics::thumbHeightForCoverWidth(coverW);
    if (coverH > coverBudget) {
      coverH = coverBudget;
      coverW = HomeCoverMetrics::thumbWidthForHeight(coverH);
      if (coverW > maxCoverW) coverW = maxCoverW;
    }
    (void)minBookStatsStackHeight(renderer);
  }

  int groupH = titleH + coverH + heroPad + belowHero;
  if (groupH > bandH) {
    const int overflow = groupH - bandH;
    preferredCoverSize(renderer, std::max(80, coverH - overflow), coverW, coverH);
    groupH = titleH + coverH + heroPad + belowHero;
    if (groupH > bandH) {
      groupH = bandH;
    }
  }

  // Bias the pack toward the top so spare band grows the hero instead of equal
  // letterboxing above the title and below lifetime. Keep a little air above.
  constexpr int kTopBiasPad = 2;
  const int spare = std::max(0, bandH - groupH);
  // Put most spare under the pack (near footer) only if we still have free space
  // after growing the cover — coverBudget already consumed band for hero size.
  const int startY = bandTop + kTopBiasPad + std::min(spare / 6, 8);
  return Rect{0, startY, renderer.getScreenWidth(), std::min(groupH, bandH - (startY - bandTop))};
}

// Cover sits on the shared left edge; portrait frame leaves room for stats.
// Contain-fit thumbs (crop=false) — no side crop; frame matches gen aspect so
// loading borders and art share the same left-aligned box.
Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect, const int coverTopY) {
  const ContentFrame frame = contentFrame(renderer);
  const int statsW = isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth;
  const int maxCoverW = std::max(80, frame.width - statsW - kCoverStatsGap);
  const int maxCoverH = std::max(120, rect.y + rect.height - coverTopY - lowerDashboardReserve(renderer));

  int coverW = 0;
  int coverH = 0;
  sizeCoverFrame(maxCoverW, maxCoverH, renderer.getScreenWidth(), coverW, coverH);
  return Rect{frame.left, coverTopY, coverW, coverH};
}

// Resolve a 1-bit thumb path for the on-screen rect.
// Shelf-sized frames prefer the compact shelf thumb (cheap blit); hero prefers
// the oversized cover-fill source. Cap Storage.exists probes — each is an SD hit.
std::string coverPathForRect(const RecentBook& book, const Rect& imageRect) {
  auto tryExists = [](const std::string& path) -> bool { return !path.empty() && Storage.exists(path.c_str()); };

  // Shelf frames are ~100–180px tall; hero is ~300–450. Threshold separates the two.
  const bool preferShelf = imageRect.height > 0 && imageRect.height <= DashboardMetrics::homeShelfThumbHeight + 48;

  // Concrete path already stored (no [HEIGHT] template) — use if it exists.
  if (!book.coverBmpPath.empty() && book.coverBmpPath.find("[HEIGHT]") == std::string::npos) {
    if (!preferShelf && tryExists(book.coverBmpPath)) {
      return book.coverBmpPath;
    }
  }

  auto firstExisting = [&](std::initializer_list<std::string> candidates) -> std::string {
    for (const std::string& path : candidates) {
      if (tryExists(path)) return path;
    }
    return {};
  };

  // Height keys that may exist on disk (gen and layout must agree; try both).
  // 1) exact layout plate height  2) width-derived 3:4 key  3) Bare-native 560
  const int heroKeyW = HomeCoverMetrics::thumbHeightForCoverWidth(std::max(80, imageRect.width));
  const int heroKeyH = imageRect.height > 0 ? imageRect.height : heroKeyW;

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (preferShelf) {
      const std::string found = firstExisting({
          epub.getThumbBmpPath(DashboardMetrics::homeShelfThumbHeight),
          epub.getThumbBmpPath(heroKeyW),
          epub.getThumbBmpPath(heroKeyH),
          epub.getThumbBmpPath(DashboardMetrics::homeCoverThumbHeight),
      });
      if (!found.empty()) return found;
    } else {
      const std::string found = firstExisting({
          epub.getThumbBmpPath(heroKeyH),
          epub.getThumbBmpPath(heroKeyW),
          epub.getThumbBmpPath(DashboardMetrics::homeCoverThumbHeight),
      });
      if (!found.empty()) return found;
    }
  }

  if (book.coverBmpPath.empty()) {
    return {};
  }

  if (preferShelf) {
    return firstExisting({
        UITheme::getCoverThumbPath(book.coverBmpPath, DashboardMetrics::homeShelfThumbHeight),
        UITheme::getCoverThumbPath(book.coverBmpPath, heroKeyW),
        UITheme::getCoverThumbPath(book.coverBmpPath, heroKeyH),
        UITheme::getCoverThumbPath(book.coverBmpPath, DashboardMetrics::homeCoverThumbHeight),
    });
  }
  return firstExisting({
      UITheme::getCoverThumbPath(book.coverBmpPath, heroKeyH),
      UITheme::getCoverThumbPath(book.coverBmpPath, heroKeyW),
      UITheme::getCoverThumbPath(book.coverBmpPath, DashboardMetrics::homeCoverThumbHeight),
  });
}

// Contain-fit a source size into the layout slot (no upscale). Hero left-aligns;
// shelf centers. Used for real thumbs and for the loading/missing plate so the
// outline matches the jacket instead of the full (often wider) layout box.
Rect fittedSizeRect(const int srcW, const int srcH, const Rect& target, const bool leftAlign = false) {
  if (srcW <= 0 || srcH <= 0 || target.width <= 0 || target.height <= 0) {
    return target;
  }
  int drawnW = srcW;
  int drawnH = srcH;
  if (srcW > target.width || srcH > target.height) {
    const float widthScale = static_cast<float>(target.width) / static_cast<float>(srcW);
    const float heightScale = static_cast<float>(target.height) / static_cast<float>(srcH);
    const float scale = std::min(widthScale, heightScale);
    drawnW = std::max(1, static_cast<int>(std::floor(static_cast<float>(srcW) * scale)));
    drawnH = std::max(1, static_cast<int>(std::floor(static_cast<float>(srcH) * scale)));
  }
  const int x = leftAlign ? target.x : target.x + (target.width - drawnW) / 2;
  const int y = target.y + (target.height - drawnH) / 2;
  return Rect{x, y, drawnW, drawnH};
}

Rect fittedBitmapRect(const Bitmap& bitmap, const Rect& target, const bool leftAlign = false) {
  return fittedSizeRect(bitmap.getWidth(), bitmap.getHeight(), target, leftAlign);
}

// Missing/loading plate: always the layout cover column (gen-native W×H from
// layoutHeroBlock). Never re-fit Bare 420×560 or a stale on-disk size that is
// wider than the column (that bled into the stats zone).
Rect expectedHeroArtRect(const Rect& slot, const RecentBook& /*book*/) {
  if (slot.width <= 0 || slot.height <= 0) {
    return slot;
  }
  return Rect{slot.x, slot.y, slot.width, slot.height};
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  // Same rect a real 1:1 gen thumb would occupy — not the full hero card.
  const Rect plate = expectedHeroArtRect(coverRect, book);
  renderer.fillRoundedRect(plate.x, plate.y, plate.width, plate.height, kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(plate.x, plate.y, plate.width, plate.height, 1, kCoverCornerRadius, true);

  constexpr int iconSize = 32;
  if (plate.width >= iconSize + 8 && plate.height >= iconSize + 8) {
    renderer.drawIcon(CoverIcon, plate.x + (plate.width - iconSize) / 2, plate.y + std::min(36, plate.height / 6),
                      iconSize);
  }

  // Only draw a title inside the plate when there is room; tiny width → lone "…".
  constexpr int textPadding = 14;
  const int textW = plate.width - textPadding * 2;
  if (textW < 40) {
    return;
  }
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title, textW, 4, EpdFontFamily::BOLD);
  if (titleLines.empty()) {
    return;
  }
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int textY = plate.y + (plate.height - static_cast<int>(titleLines.size()) * lineH) / 2;
  for (const auto& line : titleLines) {
    // Skip pure ellipsis-only stubs (looks like a stray glyph on the left).
    if (line == "\xe2\x80\xa6" || line == "...") {
      continue;
    }
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, plate.x + (plate.width - lineW) / 2, textY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    textY += lineH;
  }
}

// Hero cover art. 2-bit thumbs need HomeActivity grayscale multipass after paint
// (sleep-screen style) for real midtones — single-pass BW looks like 4-color pixel art
// if Bayer-mapped, or solid black if val<3 is used alone.
// Returns the on-screen art rect (for snapshot / multipass) — not the wider layout slot.
Rect drawBookCoverHeroStyle(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                            const Color backgroundColor) {
  if (coverRect.width <= 0 || coverRect.height <= 0) {
    return coverRect;
  }

  const int radius = std::min(kCoverCornerRadius, std::min(coverRect.width, coverRect.height) / 4);
  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (coverBmpPath.empty()) {
    const Rect plate = expectedHeroArtRect(coverRect, book);
    drawMissingBookCover(renderer, coverRect, book);
    return plate;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
    const Rect plate = expectedHeroArtRect(coverRect, book);
    drawMissingBookCover(renderer, coverRect, book);
    return plate;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    file.close();
    const Rect plate = expectedHeroArtRect(coverRect, book);
    drawMissingBookCover(renderer, coverRect, book);
    return plate;
  }

  // Art plate only; top-left aligned so the jacket shares an edge with book stats.
  Rect bitmapRect = fittedBitmapRect(bitmap, coverRect, /*leftAlign=*/true);
  bitmapRect.y = coverRect.y;  // pin to top (fittedBitmapRect vertically centers)
  const int artRadius = std::min(radius, std::min(bitmapRect.width, bitmapRect.height) / 4);
  renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius, Color::White);
  // 1:1 native blit when the thumb fits (scaling 2-bit Atkinson shows gridlines).
  const int bw = bitmap.getWidth();
  const int bh = bitmap.getHeight();
  if (bitmapRect.width == bw && bitmapRect.height == bh) {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bw, bh);
  } else {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
  }
  renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius,
                                         backgroundColor);
  renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1, artRadius, true);
  file.close();
  return bitmapRect;
}

Rect drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  return drawBookCoverHeroStyle(renderer, coverRect, book, backgroundColor);
}

void drawRightAlignedText(const GfxRenderer& renderer, const int fontId, const int rightX, const int y,
                          const char* text, const bool bold = false, const bool black = true) {
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rightX - width, y, text, black, style);
}

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}

// Prefer the page-based ETA cached when the reader last saved; weak progress
// fallback only if that cache is empty (e.g. older stats files).
bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return estimateTimeLeftFromProgress(stats.totalReadingSeconds, progressPercent, seconds);
}

bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }

  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) {
    return false;
  }

  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

// Height of one value+label pair with label pulled up under the value.
int statsPairContentHeight(const GfxRenderer& renderer) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  return std::max(1, valueLineH - kStatsValueLabelPull) + labelLineH;
}

// Y of the value for stats row `index`. Pairs stay visually grouped: tight
// value→label, then a clear inter-pair gap before the next value. Extra vertical
// room is added only between pairs (not between a value and its own label).
// Never force min gaps that would push the last pair outside spanH.
int statsPairTop(const int topY, const int spanH, const int index, const int pairH, const int rowCount) {
  if (rowCount <= 1 || index <= 0) {
    return topY;
  }
  const int contentTotal = pairH * rowCount;
  const int free = spanH - contentTotal;
  int interGap = 0;
  if (free > 0 && rowCount > 1) {
    // Distribute only free space so the stack always fits inside the box.
    interGap = free / (rowCount - 1);
    // Cap so pairs stay grouped (no huge voids between label and next value).
    interGap = std::min(interGap, 10);
  }
  return topY + index * (pairH + interGap);
}

void drawStatsRow(const GfxRenderer& renderer, const int rightX, const int y, const char* value, const char* label,
                  const bool black = true) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  drawRightAlignedText(renderer, UI_12_FONT_ID, rightX, y, value, true, black);
  // Pull label up under the value so "1h" + "Reading time" read as one unit.
  const int labelY = y + std::max(1, valueLineH - kStatsValueLabelPull);
  drawRightAlignedText(renderer, SMALL_FONT_ID, rightX, labelY, label, false, black);
}

// Reserve enough width for real right-aligned stats so the cover column cannot
// expand under the numbers (fixed 105/120 was too tight for Source Serif labels).
int bookStatsColumnWidth(const GfxRenderer& renderer) {
  int maxW = isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth;
  auto consider = [&](const int fontId, const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    if (text == nullptr || text[0] == '\0') return;
    maxW = std::max(maxW, renderer.getTextWidth(fontId, text, style));
  };
  consider(UI_12_FONT_ID, "999h 99m", EpdFontFamily::BOLD);
  consider(UI_12_FONT_ID, "999 days", EpdFontFamily::BOLD);
  consider(UI_12_FONT_ID, "100%", EpdFontFamily::BOLD);
  consider(UI_12_FONT_ID, "Dec 99", EpdFontFamily::BOLD);
  consider(SMALL_FONT_ID, tr(STR_STATS_TIME_LBL));
  consider(SMALL_FONT_ID, tr(STR_TIME_LEFT));
  consider(SMALL_FONT_ID, tr(STR_STATS_PROGRESS_LBL));
  consider(SMALL_FONT_ID, tr(STR_STATS_DAILY_AVG_LBL));
  consider(SMALL_FONT_ID, tr(STR_STATS_PAGES_PER_MIN));
  consider(SMALL_FONT_ID, tr(STR_STATS_SESSIONS_LBL));
  consider(SMALL_FONT_ID, tr(STR_STATS_AVG_SESSION_LBL));
  consider(SMALL_FONT_ID, tr(STR_STATS_PAGES_LBL));
  consider(SMALL_FONT_ID, tr(STR_STATS_FINISHED_DATE));
  consider(SMALL_FONT_ID, tr(STR_STATS_EST_FINISH_DATE));
  {
    char started[48];
    snprintf(started, sizeof(started), "%s Dec 99", tr(STR_STATS_STARTED));
    consider(SMALL_FONT_ID, started);
  }
  return maxW + 4;
}

// Right edge for the book-stats column (values right-align to this x).
int bookStatsRightX(const GfxRenderer& renderer, const Rect& coverRect) {
  (void)coverRect;
  const ContentFrame frame = contentFrame(renderer);
  // Current and card: flush to the content-frame right edge (card insets pad inside).
  if (dashboardLayoutVariant() == 3) {
    return frame.right() - 1 - kHeroCardPad;
  }
  return frame.right() - 1;
}

void drawDashboardStats(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                        const float progressPercent, const bool black = true) {
  // Vertical span matches the cover layout rect (same box as drawBookCover's frame).
  const int rightX = bookStatsRightX(renderer, coverRect);
  const int pairH = statsPairContentHeight(renderer);
  const int spanH = std::max(pairH, coverRect.height);
  const bool showRtcStats = gpio.deviceIsX3();
  const int rowCount = showRtcStats ? kStatsRowCount : kStatsRowCountX4;
  const BookReadingStats emptyStats{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : emptyStats;
  char value[40];
  char label[40];
  char startedDate[24];
  char finishDate[24];
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
  ReadingStatsDateTime today;
  const bool hasToday = showRtcStats && getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = bookStats.isCompleted && bookStats.finishedDate.isValid()
                                       ? bookStats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = bookStats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(bookStats.startDate, endDate) : 0;

  int rowIndex = 0;
  auto rowY = [&]() { return statsPairTop(coverRect.y, spanH, rowIndex, pairH, rowCount); };

  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, value, sizeof(value));
  drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_TIME_LBL), black);

  ++rowIndex;
  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY(), value, tr(STR_TIME_LEFT), black);

  ++rowIndex;
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_PROGRESS_LBL), black);

  if (showRtcStats) {
    ++rowIndex;
    if (hasDaySpan) {
      const uint16_t dailyAverageDays = std::max<uint16_t>(1, daysReading);
      BookReadingStats::formatDuration(bookStats.totalReadingSeconds / dailyAverageDays, value, sizeof(value));
    } else {
      snprintf(value, sizeof(value), "-");
    }
    drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_DAILY_AVG_LBL), black);
  }

  ++rowIndex;
  snprintf(value, sizeof(value), "%.1f", pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));
  drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_PAGES_PER_MIN), black);

  if (!showRtcStats) {
    ++rowIndex;
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(bookStats.sessionCount));
    drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_SESSIONS_LBL), black);

    ++rowIndex;
    const uint32_t avgSeconds = bookStats.sessionCount > 0 ? bookStats.totalReadingSeconds / bookStats.sessionCount : 0;
    BookReadingStats::formatDuration(avgSeconds, value, sizeof(value));
    drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_AVG_SESSION_LBL), black);

    ++rowIndex;
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(bookStats.totalPagesTurned));
    drawStatsRow(renderer, rightX, rowY(), value, tr(STR_STATS_PAGES_LBL), black);
    return;
  }

  ++rowIndex;
  if (hasDaySpan) {
    snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  formatReadingStatsShortDate(bookStats.startDate, startedDate, sizeof(startedDate));
  snprintf(label, sizeof(label), "%s %s", tr(STR_STATS_STARTED), startedDate);
  drawStatsRow(renderer, rightX, rowY(), value, label, black);

  ++rowIndex;
  ReadingStatsDate finishDisplayDate;
  if (bookStats.isCompleted) {
    finishDisplayDate = bookStats.finishedDate;
  } else if (hasToday && hasEstimate) {
    if (!estimateFinishDateFromDailyPace(bookStats, today, estimatedSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, estimatedSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, finishDate, sizeof(finishDate));
  drawStatsRow(renderer, rightX, rowY(), finishDate,
               bookStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE), black);
}

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return tr(STR_STATS_NEW_READER);
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

const uint8_t* readerTypeIcon(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return Book24Icon;
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return Book24Icon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
    default:
      return NightReaderIcon;
  }
}

void formatStreakStat(const GlobalReadingStats* globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }
  if (globalStats == nullptr) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats->currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }
  snprintf(buf, len, tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
}

void drawIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int iconX, const int centerY,
                   const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  // 1.5 GfxRenderer has single-size drawIcon only (no drawIconInverted / WxH).
  renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize);
  renderer.drawText(UI_10_FONT_ID, iconX + kFooterIconSize + kFooterIconTextGap, centerY - lineH / 2,
                    visibleLabel.c_str(), !inverted);
}

void drawRightAlignedIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int rightX, const int centerY,
                               const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textX = rightX - labelW;
  const int iconX = textX - kFooterIconTextGap - kFooterIconSize;
  renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize);
  renderer.drawText(UI_10_FONT_ID, textX, centerY - lineH / 2, visibleLabel.c_str(), !inverted);
}

void drawLeftAnchoredFooterStat(const GfxRenderer& renderer, const int labelX, const int centerY, const int maxTextW,
                                const char* value, const char* label, const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

void drawRightAnchoredFooterStat(const GfxRenderer& renderer, const int labelRightX, const int centerY,
                                 const int maxTextW, const char* value, const char* label,
                                 const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int labelX = labelRightX - labelW;
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

// Achievements under the cover (streaks / reader type).
// X3: day streak + reader type (RTC-based).
// X4: sessions + pages turned (no calendar / time-of-day).
// bandTopY: absolute top of the meta band (caller centers it between hero & lifetime).
// Returns Y just below this band for the lifetime card.
int drawMetaStatsUnderCover(const GfxRenderer& renderer, const int bandTopY, const GlobalReadingStats* globalStats,
                            const BookReadingStats* bookStats = nullptr, const float progressPercent = -1.0f,
                            const bool inverted = false) {
  const ContentFrame frame = contentFrame(renderer);
  const int bandH = kMetaBandH;
  const int topY = bandTopY;
  const int centerY = topY + bandH / 2;
  constexpr int kAchGap = 6;  // space between icon and text

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = centerY - lineH / 2;
  const int iconY = centerY - kFooterIconSize / 2;

  const int half0 = frame.width / 2;
  const int half1 = frame.width - half0;
  const int col0X = frame.left;
  const int col1X = frame.left + half0;

  auto drawCenteredAchievement = [&](const int colX, const int colW, const uint8_t* icon, const char* label) {
    const int maxTextW = std::max(1, colW - kFooterIconSize - kAchGap - 8);
    const std::string visible = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, visible.c_str());
    const int groupW = kFooterIconSize + kAchGap + textW;
    const int groupX = colX + std::max(0, (colW - groupW) / 2);
    renderer.drawIcon(icon, groupX, iconY, kFooterIconSize);
    renderer.drawText(UI_10_FONT_ID, groupX + kFooterIconSize + kAchGap, textY, visible.c_str(), !inverted);
  };

  if (gpio.deviceIsX3()) {
    char streakBuf[48];
    formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));
    drawCenteredAchievement(col0X, half0, StreakIcon, streakBuf);
    drawCenteredAchievement(col1X, half1, readerTypeIcon(globalStats), readerTypeLabel(globalStats));
  } else {
    // X4: fill the band with session/page counts + a glanceable progress chip.
    const GlobalReadingStats emptyG{};
    const GlobalReadingStats& g = globalStats != nullptr ? *globalStats : emptyG;
    const BookReadingStats emptyB{};
    const BookReadingStats& b = bookStats != nullptr ? *bookStats : emptyB;

    char leftBuf[48];
    char rightBuf[48];
    if (b.sessionCount > 0) {
      snprintf(leftBuf, sizeof(leftBuf), "%u %s", static_cast<unsigned>(b.sessionCount), tr(STR_STATS_SESSIONS_LBL));
    } else if (g.totalSessions > 0) {
      snprintf(leftBuf, sizeof(leftBuf), "%lu %s", static_cast<unsigned long>(g.totalSessions),
               tr(STR_STATS_SESSIONS_LBL));
    } else {
      snprintf(leftBuf, sizeof(leftBuf), "- %s", tr(STR_STATS_SESSIONS_LBL));
    }

    if (progressPercent >= 0.0f) {
      snprintf(rightBuf, sizeof(rightBuf), "%d%% %s", static_cast<int>(progressPercent + 0.5f),
               tr(STR_STATS_PROGRESS_LBL));
    } else if (b.totalPagesTurned > 0) {
      snprintf(rightBuf, sizeof(rightBuf), "%lu %s", static_cast<unsigned long>(b.totalPagesTurned),
               tr(STR_STATS_PAGES_SHORT_LBL));
    } else {
      snprintf(rightBuf, sizeof(rightBuf), "- %s", tr(STR_STATS_PROGRESS_LBL));
    }

    drawCenteredAchievement(col0X, half0, Book24Icon, leftBuf);
    drawCenteredAchievement(col1X, half1, Book24Icon, rightBuf);
  }

  return topY + bandH + 1;
}

// Lifetime value+label cell: value and label each centered in the cell.
// Values use UI_10 (one step under the book-column UI_12). Long labels are
// truncated by draw path if a column is too narrow.
void drawDashboardLifetimeStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h,
                                   const char* value, const char* label, const bool black) {
  constexpr int kValueFont = UI_10_FONT_ID;
  constexpr int kLabelFont = SMALL_FONT_ID;
  constexpr int kPadX = 2;
  constexpr int kValueLabelGap = 1;
  const int textW = std::max(1, w - kPadX * 2);
  const int valueLineH = renderer.getLineHeight(kValueFont);
  const int labelLineH = renderer.getLineHeight(kLabelFont);
  const int totalTextH = valueLineH + kValueLabelGap + labelLineH;
  // Pack toward top of cell so the two lifetime rows sit closer together.
  const int textY = y + kLifetimeCellPadY + std::max(0, (h - totalTextH - kLifetimeCellPadY * 2) / 4);
  const std::string visValue = renderer.truncatedText(kValueFont, value, textW, EpdFontFamily::BOLD);
  const std::string visLabel = renderer.truncatedText(kLabelFont, label, textW);
  const int valueW = renderer.getTextWidth(kValueFont, visValue.c_str(), EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(kLabelFont, visLabel.c_str());
  renderer.drawText(kValueFont, x + (w - valueW) / 2, textY, visValue.c_str(), black, EpdFontFamily::BOLD);
  renderer.drawText(kLabelFont, x + (w - labelW) / 2, textY + valueLineH + kValueLabelGap, visLabel.c_str(), black);
}

// Lifetime stats card.
// Header: centered "Lifetime Stats".
// Body 2x3:
//   X3: Sessions, Time, Pages/Min / Avg Session, Completed, Longest Streak
//   X4: Sessions, Time, Pages/Min / Avg Session, Completed, Pages Turned
//       (no streak — requires RTC calendar days).
void drawLifetimeStatsCard(const GfxRenderer& renderer, const Rect& cardRect, const GlobalReadingStats* globalStats,
                           const bool black = true) {
  if (cardRect.width < 80 || cardRect.height < 60) return;

  const GlobalReadingStats empty{};
  const GlobalReadingStats& stats = globalStats != nullptr ? *globalStats : empty;

  constexpr int kTitlePadX = 8;
  constexpr int kBodyInsetX = 2;  // equal left/right inset so the grid sits centered
  constexpr int kColCount = 3;
  constexpr int kRowCount = 2;
  // Squish header: minimal air around Life Stats title.
  const int titleFontH = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleH = std::min(std::max(kLifetimeTitleH, titleFontH + 2), std::max(titleFontH + 2, cardRect.height / 5));
  const int titleTextY = cardRect.y + (titleH - titleFontH) / 2;

  // Card chrome — match book cover: rounded corners + slightly thicker stroke.
  constexpr int kCardStroke = 2;
  const int radius = std::min(kCoverCornerRadius, std::min(cardRect.width, cardRect.height) / 4);
  renderer.drawRoundedRect(cardRect.x, cardRect.y, cardRect.width, cardRect.height, kCardStroke, radius, black);
  // Title divider inset so ends sit inside the rounded sides.
  const int divInset = std::max(2, radius / 2);
  const int divY = cardRect.y + titleH;
  for (int t = 0; t < kCardStroke; ++t) {
    renderer.drawLine(cardRect.x + divInset, divY + t, cardRect.x + cardRect.width - 1 - divInset, divY + t, black);
  }

  // Header: single centered title.
  const int titleMaxW = std::max(1, cardRect.width - kTitlePadX * 2);
  const std::string titleVis =
      renderer.truncatedText(UI_10_FONT_ID, tr(STR_STATS_ALL_TIME), titleMaxW, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(UI_10_FONT_ID, titleVis.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, cardRect.x + (cardRect.width - titleW) / 2, titleTextY, titleVis.c_str(), black,
                    EpdFontFamily::BOLD);

  // Body clears title divider and bottom stroke (Avg Session labels must not sit on the line).
  constexpr int kBodyInsetTop = 1;
  constexpr int kBodyInsetBottom = 11;
  const int bodyY = cardRect.y + titleH + kBodyInsetTop;
  const int bodyH = std::max(1, cardRect.height - titleH - kBodyInsetTop - kBodyInsetBottom);
  // Centered grid: equal side insets, then remainder pixels spread across columns
  // so no column is wider than the others by more than 1px.
  const int gridW = std::max(kColCount, cardRect.width - kBodyInsetX * 2);
  const int baseColW = gridW / kColCount;
  const int colRem = gridW % kColCount;
  int colW[kColCount];
  int colX[kColCount];
  int x = cardRect.x + kBodyInsetX;
  for (int i = 0; i < kColCount; ++i) {
    colW[i] = baseColW + (i < colRem ? 1 : 0);
    colX[i] = x;
    x += colW[i];
  }
  // Equal row heights; leftover body pixels split top/bottom for vertical centering.
  const int baseRowH = bodyH / kRowCount;
  const int rowRem = bodyH % kRowCount;
  const int rowH0 = baseRowH + (rowRem > 0 ? 1 : 0);
  const int rowH1 = baseRowH + (rowRem > 1 ? 1 : 0);
  // If bodyH % 2 == 1, first row got the extra pixel; grid still fills the body.
  const int rowY0 = bodyY;
  const int rowY1 = bodyY + rowH0;

  char buf[40];

  auto cell = [&](const int col, const int rowY, const int rowH, const char* value, const char* label) {
    drawDashboardLifetimeStatCell(renderer, colX[col], colW[col], rowY, rowH, value, label, black);
  };

  // Row 1: Sessions, Time, Pages/Min
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalSessions));
  cell(0, rowY0, rowH0, buf, tr(STR_STATS_SESSIONS_LBL));

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  cell(1, rowY0, rowH0, buf, tr(STR_STATS_TIME_LBL));

  snprintf(buf, sizeof(buf), "%.1f", pagesPerMinute(stats.totalPagesTurned, stats.totalReadingSeconds));
  cell(2, rowY0, rowH0, buf, tr(STR_STATS_PAGES_PER_MIN));

  // Row 2: Avg Session, Completed, (X3 Longest Streak | X4 Pages Turned)
  const uint32_t avgSecs = stats.totalSessions > 0 ? stats.totalReadingSeconds / stats.totalSessions : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  cell(0, rowY1, rowH1, buf, tr(STR_STATS_AVG_SESSION_LBL));

  if (stats.completedBooks > 0) {
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.completedBooks));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(1, rowY1, rowH1, buf, tr(STR_STATS_COMPLETED_LBL));

  if (gpio.deviceIsX3()) {
    const uint16_t longest = stats.displayLongestReadingStreak();
    if (longest > 0) {
      // Unit in the same bold value font as the number: "11 Days".
      snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
    } else {
      snprintf(buf, sizeof(buf), "-");
    }
    cell(2, rowY1, rowH1, buf, tr(STR_STATS_LONGEST_STREAK_LBL));
  } else {
    if (stats.totalPagesTurned > 0) {
      snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
    } else {
      snprintf(buf, sizeof(buf), "-");
    }
    cell(2, rowY1, rowH1, buf, tr(STR_STATS_PAGES_LBL));
  }
}
// Lifetime card: fixed compact height at pack bottom. Leftover height stays with the hero.
// Returns the card top Y (or -1 if not drawn).
int drawLifetimeStatsUnderMeta(const GfxRenderer& renderer, const Rect& contentRect, const int heroBottomY,
                               const GlobalReadingStats* globalStats, const bool black = true) {
  const ContentFrame frame = contentFrame(renderer);
  const int lifeH = minLifetimeCardHeight(renderer);
  const int packBottom = contentRect.y + contentRect.height - kLifetimeStatsBottomPad;
  const int minTop = heroBottomY + kCoverMetaGap;
  // Exact min height at footer — do not stretch upward into hero space.
  int finalTop = packBottom - lifeH;
  if (finalTop < minTop) {
    finalTop = minTop;
  }
  const int drawH = packBottom - finalTop;
  if (drawH < 56) return -1;

  const Rect cardRect{frame.left, finalTop, frame.width, drawH};
  drawLifetimeStatsCard(renderer, cardRect, globalStats, black);
  return finalTop;
}

// Cheap selection mark: solid underline only (no outer ring / extra rounded stroke).
void drawShelfSelectionUnderline(const GfxRenderer& renderer, const Rect& coverRect, const bool black) {
  if (coverRect.width <= 0 || coverRect.height <= 0) return;
  constexpr int kBarH = 3;
  const int barW = std::max(16, (coverRect.width * 3) / 4);
  const int barX = coverRect.x + (coverRect.width - barW) / 2;
  const int barY = coverRect.y + coverRect.height + 2;
  renderer.fillRect(barX, barY, barW, kBarH, black);
}

// Lightweight shelf cover: full-bleed blit + thin border. Skips corner-mask passes
// used on the hero (those are costly ×4 every Left/Right redraw).
void drawBookCoverShelfLite(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                            const Color backgroundColor) {
  if (coverRect.width <= 0 || coverRect.height <= 0) return;

  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (coverBmpPath.empty()) {
    drawMissingBookCover(renderer, coverRect, book);
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
    drawMissingBookCover(renderer, coverRect, book);
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    file.close();
    drawMissingBookCover(renderer, coverRect, book);
    return;
  }

  // Flat plate + contain-fit blit (small shelf frames; multipass is hero-only).
  renderer.fillRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, backgroundColor == Color::Black);
  const Rect bitmapRect = fittedBitmapRect(bitmap, coverRect);
  renderer.fillRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, /*black=*/false);
  renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
  const int radius = std::min(kCoverCornerRadius, std::min(coverRect.width, coverRect.height) / 4);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, radius, true);
  file.close();
}

// Stats shelf: fixed recent order, hard-capped at kRecentsShelfVisible (4).
// Left/Right moves underline selection; hero mirrors the selected book.
void drawRecentBooksShelf(const GfxRenderer& renderer, const Rect& pack, const int shelfTopY,
                          const std::vector<RecentBook>& recentBooks, const int focusIndex, const bool black = true) {
  if (recentBooks.empty()) return;

  const ContentFrame frame = contentFrame(renderer);
  const int shelfOuterH = recentsShelfHeight(renderer) - kRecentsShelfBottomPad;
  if (shelfOuterH < 40) return;

  // Same horizontal band + rounded stroke as the hero card above.
  const Rect shelfCard{frame.left, shelfTopY, frame.width, shelfOuterH};
  const int cardRadius = std::min(kCoverCornerRadius, std::min(shelfCard.width, shelfCard.height) / 4);
  renderer.drawRoundedRect(shelfCard.x, shelfCard.y, shelfCard.width, shelfCard.height, /*lineWidth=*/2, cardRadius,
                           black);

  const int pad = kHeroCardPad;
  const int innerW = std::max(1, shelfCard.width - pad * 2);
  // Space under covers for the selection underline.
  constexpr int kSelectBarReserve = 6;
  const int innerH = std::max(1, shelfCard.height - pad * 2 - kSelectBarReserve);
  const int innerLeft = shelfCard.x + pad;
  const int innerTop = shelfCard.y + pad;

  const int count = static_cast<int>(recentBooks.size());
  const int focus = std::clamp(focusIndex, 0, count - 1);
  // Hard cap: first N recents only (N = 4). No sliding window, no 1/N badge.
  const int shelfCount = std::min(kRecentsShelfVisible, count);

  const int totalGap = kRecentsShelfGap * std::max(0, shelfCount - 1);
  const int slotW = std::max(30, (innerW - totalGap) / std::max(1, shelfCount));
  int coverH = innerH;
  int coverW =
      std::max(1, (coverH * DashboardMetrics::homeCoverImageWidth + DashboardMetrics::homeCoverImageHeight / 2) /
                      DashboardMetrics::homeCoverImageHeight);
  if (coverW > slotW) {
    coverW = slotW;
    coverH = std::max(1, (coverW * DashboardMetrics::homeCoverImageHeight + DashboardMetrics::homeCoverImageWidth / 2) /
                             DashboardMetrics::homeCoverImageWidth);
    if (coverH > innerH) coverH = innerH;
  }

  const int rowW = shelfCount * coverW + totalGap;
  int x = innerLeft + std::max(0, (innerW - rowW) / 2);
  const int y = innerTop + std::max(0, (innerH - coverH) / 2);

  for (int i = 0; i < shelfCount; ++i) {
    const Rect r{x, y, coverW, coverH};
    drawBookCoverShelfLite(renderer, r, recentBooks[static_cast<size_t>(i)], black ? Color::White : Color::Black);
    if (i == focus) {
      drawShelfSelectionUnderline(renderer, r, black);
    }
    x += coverW + kRecentsShelfGap;
  }
  (void)pack;
}

// Hero block (cover + optional card chrome).
struct HeroLayout {
  Rect coverRect{};
  Rect cardRect{};
  int heroBottomY = 0;
};

HeroLayout layoutHeroBlock(const GfxRenderer& renderer, const Rect& contentRect, const int coverTopY) {
  HeroLayout hero;
  hero.coverRect = coverRectForScreen(renderer, contentRect, coverTopY);
  hero.heroBottomY = hero.coverRect.y + hero.coverRect.height;

  if (dashboardLayoutVariant() != 3) {
    return hero;
  }

  const int statsW = bookStatsColumnWidth(renderer);
  const int pad = kHeroCardPad;
  const int leftInset = kHeroCoverLeftInset;
  const ContentFrame frame = contentFrame(renderer);
  const int cardX = frame.left;
  const int cardY = coverTopY;
  const int cardW = std::max(1, frame.width);
  // Cover column ends before the reserved stats strip (never under the numbers).
  // [leftInset][cover 3:4][gap][statsW][pad]
  // Gen height MUST match HomeCoverMetrics::dashboardHeroThumbHeight / HomeActivity
  // so thumb_cN_<H>.bmp is found after generateThumbBmp(H).
  const int coverColLeft = cardX + leftInset;
  const int coverColRight = cardX + cardW - pad - kCoverStatsGap - statsW;
  const int maxCoverW = std::max(40, coverColRight - coverColLeft);
  const int maxCardH = std::max(80, contentRect.y + contentRect.height - coverTopY - lowerDashboardReserve(renderer));
  const int maxArtH = std::max(60, maxCardH - pad * 2);

  // Prefer shared helper (same as gen); then clamp into this card's column.
  int coverH = HomeCoverMetrics::dashboardHeroThumbHeight(renderer.getScreenWidth(), maxArtH);
  int coverW = HomeCoverMetrics::thumbWidthForHeight(coverH);
  if (coverW > maxCoverW) {
    coverW = maxCoverW;
    coverH = HomeCoverMetrics::thumbHeightForCoverWidth(coverW);
    if (coverH > maxArtH) {
      coverH = maxArtH;
      coverW = HomeCoverMetrics::thumbWidthForHeight(coverH);
      if (coverW > maxCoverW) coverW = maxCoverW;
    }
  }
  coverW = std::max(coverW, 40);
  coverH = std::max(coverH, 60);
  if (coverColLeft + coverW > coverColRight) {
    coverW = std::max(40, coverColRight - coverColLeft);
  }

  // Card grows with the book-stat stack; cover plate stays the gen column size.
  const int statsMinH = minBookStatsStackHeight(renderer);
  const int innerH = std::max(coverH, std::min(statsMinH, maxArtH));
  const int cardH = std::max(coverH, innerH) + pad * 2;
  hero.cardRect = Rect{cardX, cardY, cardW, cardH};
  hero.coverRect = Rect{coverColLeft, cardY + pad, coverW, coverH};
  hero.heroBottomY = cardY + cardH;
  return hero;
}

void drawHeroCardChrome(const GfxRenderer& renderer, const Rect& cardRect, const bool black) {
  if (cardRect.width <= 0 || cardRect.height <= 0) return;
  const int radius = std::min(kCoverCornerRadius, std::min(cardRect.width, cardRect.height) / 4);
  renderer.drawRoundedRect(cardRect.x, cardRect.y, cardRect.width, cardRect.height, /*lineWidth=*/2, radius, black);
}

void drawEmptyCoverPlate(const GfxRenderer& renderer, const Rect& coverRect) {
  if (coverRect.width <= 0 || coverRect.height <= 0) return;
  // Same plate as a real cover / missing-cover outline (cover column only).
  drawMissingBookCover(renderer, coverRect, RecentBook{});
}

// Full dashboard shell: title → hero → (meta → lifetime | recents shelf).
// Same path on X3 and X4 (one dual-device binary); only RTC-backed stat *values*
// branch inside drawDashboardStats / meta / lifetime.
void drawDashboardHomeComposition(GfxRenderer& renderer, const RecentBook& book, const bool hasBook,
                                  bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                  BaseTheme::StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats,
                                  const float progressPercent, const GlobalReadingStats* globalStats,
                                  const std::vector<RecentBook>* allRecents = nullptr, const int recentsFocus = 0) {
  // Dashboard home is always portrait (matches HomeActivity::onEnter).
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  Rect pack = centeredDashboardPackRect(renderer, screen, book);
  // Degenerate pack (orientation glitch / tiny band): fall back to a safe band
  // under chrome so empty-home never draws only a top-left outline.
  if (pack.height < 200 || pack.y < 0 || pack.y + pack.height > screen.height) {
    int bandTop = 0;
    int bandBottom = 0;
    homeContentBand(renderer, bandTop, bandBottom);
    pack = Rect{0, bandTop, screen.width, std::max(200, bandBottom - bandTop)};
  }
  const int coverTopY = drawTopBookTitle(renderer, pack, book, /*black=*/true);
  const HeroLayout hero = layoutHeroBlock(renderer, pack, coverTopY);
  const Rect coverRect = hero.coverRect;

  drawHeroCardChrome(renderer, hero.cardRect, /*black=*/true);

  if (hasBook) {
    if (!coverRendered || !bufferRestored) {
      // Snapshot the art rect (not the wider layout slot) so multipass matches the jacket.
      const Rect artRect = drawBookCover(renderer, coverRect, book, Color::White);
      coverBufferStored = storeCoverBuffer(artRect.x, artRect.y, artRect.width, artRect.height);
      coverRendered = coverBufferStored;
    }
  } else {
    drawEmptyCoverPlate(renderer, coverRect);
    coverRendered = false;
    coverBufferStored = false;
  }

  // Stats still span the layout cover column height (card rhythm), not art letterbox.
  drawDashboardStats(renderer, coverRect, hasBook ? stats : nullptr, hasBook ? progressPercent : -1.0f);

  if (isDashboardRecentsTheme()) {
    // Shelf replaces lifetime — covers only (no titles under thumbs).
    const int shelfTop = hero.heroBottomY + kCoverMetaGap;
    if (allRecents != nullptr && !allRecents->empty()) {
      drawRecentBooksShelf(renderer, pack, shelfTop, *allRecents, recentsFocus, /*black=*/true);
    }
  } else {
    // Lifetime only — home streaks removed so the hero can use that vertical space.
    drawLifetimeStatsUnderMeta(renderer, pack, hero.heroBottomY, globalStats, /*black=*/true);
  }

  renderer.setOrientation(savedOrientation);
}
}  // namespace

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                         const BookReadingStats* stats, const float progressPercent,
                                         const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)currentChapterTitle;
  (void)rect;

  // One code path: empty home uses the same pack/chrome/stats shell as a real book.
  RecentBook placeholder;
  if (recentBooks.empty()) {
    placeholder.title = tr(STR_NO_RECENT_BOOKS);
  }
  const int focus = recentBooks.empty() ? 0 : std::clamp(selectorIndex, 0, static_cast<int>(recentBooks.size()) - 1);
  const RecentBook& book = recentBooks.empty() ? placeholder : recentBooks[static_cast<size_t>(focus)];
  drawDashboardHomeComposition(renderer, book, /*hasBook=*/!recentBooks.empty(), coverRendered, coverBufferStored,
                               bufferRestored, storeCoverBuffer, stats, progressPercent, globalStats, &recentBooks,
                               focus);
}

void DashboardTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                     const GlobalReadingStats* globalStats, const float progressPercent,
                                     const char* currentChapterTitle, const bool inverted) const {
  (void)currentChapterTitle;
  renderer.clearScreen(inverted ? 0xFF : 0x00);

  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const Rect pack = centeredDashboardPackRect(renderer, screen, book);
  const int coverTopY = drawTopBookTitle(renderer, pack, book, /*black=*/!inverted);
  const HeroLayout sleepHero = layoutHeroBlock(renderer, pack, coverTopY);
  const Rect& coverRect = sleepHero.coverRect;
  drawHeroCardChrome(renderer, sleepHero.cardRect, /*black=*/!inverted);
  drawBookCover(renderer, coverRect, book, inverted ? Color::White : Color::Black);
  drawDashboardStats(renderer, coverRect, stats, progressPercent, inverted);
  drawLifetimeStatsUnderMeta(renderer, pack, sleepHero.heroBottomY, globalStats, /*black=*/!inverted);
}