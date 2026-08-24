#include "BareTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/CrossPointPaths.h"
#include "util/StringUtils.h"

namespace {
// Mockup book title/author: Source Serif 4. Footer chrome via MinimalTheme.
constexpr int kTitleFontId = SOURCESERIF4_18_FONT_ID;
constexpr int kAuthorFontId = SOURCESERIF4_14_FONT_ID;

constexpr int kCoverCornerRadius = 4;
// Narrow side margin so the cover can use almost the full screen width.
constexpr int kSideInset = 12;
constexpr int kTopPadNoChrome = 16;    // air above cover when battery + clock are hidden
constexpr int kTopPadWithChrome = 10;  // gap under battery/clock row before cover zone
// Tight title→author so the pair reads as one unit.
constexpr int kTitleAuthorGap = 3;
// Gap between cover bottom and title (title/author stay tight under the jacket).
constexpr int kMinGapCoverToText = 10;
constexpr int kTitleMaxLines = 3;  // full-width wrap; long titles must not ellipsize early
constexpr int kAuthorMaxLines = 2;

bool bareShowsBattery() { return SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY); }

bool bareShowsClock() { return SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK); }

// Y where content may begin: under optional battery/clock chrome.
int bareContentTopY() {
  if (!bareShowsBattery() && !bareShowsClock()) {
    return kTopPadNoChrome;
  }
  // Match HomeActivity bare header height (battery row).
  return BaseTheme::kTopChromeBatteryY +
         std::max(BareMetrics::values.batteryHeight + 8, BareMetrics::values.statusBarVerticalMargin) +
         kTopPadWithChrome;
}

void drawMissingCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

  constexpr int iconSize = 32;
  renderer.drawIcon(CoverIcon, coverRect.x + (coverRect.width - iconSize) / 2,
                    coverRect.y + coverRect.height / 3 - iconSize / 2, iconSize);

  constexpr int textPadding = 12;
  const int textW = coverRect.width - textPadding * 2;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto lines = renderer.wrappedText(UI_12_FONT_ID, title, textW, 3, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int y = coverRect.y + coverRect.height / 2;
  for (const auto& line : lines) {
    const int lw = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, coverRect.x + (coverRect.width - lw) / 2, y, line.c_str(), true,
                      EpdFontFamily::BOLD);
    y += lineH;
  }
}

// Prefer Bare-native 420×560 1:1. Fall back to other heights so a just-flashed
// device still shows something while c30_560 regenerates.
std::string coverPathForBook(const RecentBook& book) {
  auto firstExisting = [](std::initializer_list<std::string> candidates) -> std::string {
    for (const std::string& path : candidates) {
      if (!path.empty() && Storage.exists(path.c_str())) {
        return path;
      }
    }
    return {};
  };

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, CrossPointPaths::kPackageCacheRoot);
    const std::string found = firstExisting({
        epub.getThumbBmpPath(BareMetrics::homeCoverThumbHeight),
        epub.getThumbBmpPath(BareMetrics::homeCoverImageHeight),
    });
    if (!found.empty()) return found;
  }

  return firstExisting({
      UITheme::getCoverThumbPath(book.coverBmpPath, BareMetrics::homeCoverThumbHeight),
      UITheme::getCoverThumbPath(book.coverBmpPath, BareMetrics::homeCoverImageHeight),
      book.coverBmpPath.find("[HEIGHT]") == std::string::npos ? book.coverBmpPath : std::string{},
  });
}

// Contain-fit full jacket; prefer 1:1 native blit. Returns art rect for multipass snapshot.
Rect drawCoverImage(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  const std::string coverBmpPath = coverPathForBook(book);
  if (coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) {
    drawMissingCover(renderer, coverRect, book);
    return coverRect;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
    drawMissingCover(renderer, coverRect, book);
    return coverRect;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    file.close();
    drawMissingCover(renderer, coverRect, book);
    return coverRect;
  }

  const int bw = bitmap.getWidth();
  const int bh = bitmap.getHeight();

  // Contain-fit into the aspect frame (no crop). Never upscale dither.
  // Prefer 1:1 when the Bare plate matches gen (420×560).
  int drawnW = bw;
  int drawnH = bh;
  if (bw > coverRect.width || bh > coverRect.height) {
    const float widthScale = static_cast<float>(coverRect.width) / static_cast<float>(bw);
    const float heightScale = static_cast<float>(coverRect.height) / static_cast<float>(bh);
    const float scale = std::min(widthScale, heightScale);
    drawnW = std::max(1, static_cast<int>(std::floor(bw * scale)));
    drawnH = std::max(1, static_cast<int>(std::floor(bh * scale)));
  }
  const Rect bitmapRect{coverRect.x + (coverRect.width - drawnW) / 2, coverRect.y + (coverRect.height - drawnH) / 2,
                        drawnW, drawnH};
  const int artRadius = std::min(kCoverCornerRadius, std::min(bitmapRect.width, bitmapRect.height) / 4);
  renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius, Color::White);
  if (drawnW == bw && drawnH == bh) {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bw, bh);
  } else {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, drawnW, drawnH);
  }
  renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius,
                                         Color::White);
  renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1, artRadius, true);
  file.close();
  return bitmapRect;
}

// Draw centered wrapped lines; returns the Y just past the last line.
int drawCenteredWrapped(const GfxRenderer& renderer, const int fontId, const int centerX, int y, const int maxWidth,
                        const char* text, const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !*text) return y;
  auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  const int lineH = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    const int lw = renderer.getTextWidth(fontId, line.c_str(), style);
    renderer.drawText(fontId, centerX - lw / 2, y, line.c_str(), true, style);
    y += lineH;
  }
  return y;
}

// Measure wrapped block height without drawing.
int measureWrappedHeight(const GfxRenderer& renderer, const int fontId, const int maxWidth, const char* text,
                         const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !*text) return 0;
  const auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  return static_cast<int>(lines.size()) * renderer.getLineHeight(fontId);
}
}  // namespace

void BareTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int /*selectorIndex*/, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                    const BookReadingStats* /*stats*/, float /*progressPercent*/,
                                    const GlobalReadingStats* /*globalStats*/,
                                    const char* /*currentChapterTitle*/) const {
  (void)rect;

  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const auto& metrics = BareMetrics::values;

  // Leave room for battery/clock when the user has them enabled; otherwise a
  // small top pad so the cover sits high but not under the bezel.
  const int contentTop = bareContentTopY();
  const int footerH = metrics.buttonHintsHeight;

  if (recentBooks.empty()) {
    const char* msg = tr(STR_NO_OPEN_BOOK);
    const int lw = renderer.getTextWidth(UI_12_FONT_ID, msg);
    const int freeH = pageH - contentTop - footerH;
    renderer.drawText(UI_12_FONT_ID, (pageW - lw) / 2, contentTop + freeH / 2, msg, true);
    coverRendered = true;
    coverBufferStored = false;
    return;
  }

  const RecentBook& book = recentBooks[0];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  // Calibre often stores "Last, First"; show natural "First Last" on Bare.
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const char* author = authorDisplay.empty() ? nullptr : authorDisplay.c_str();

  // Dead band between top chrome / status bar and footer menu.
  // Cover + title + author are one group, vertically centered in this band.
  const int bandTop = contentTop;
  const int bandBottom = pageH - footerH;
  const int bandH = std::max(1, bandBottom - bandTop);

  // Title uses full content width (up to 3 lines) so long names are not cut off.
  const int textMaxW = std::max(40, pageW - kSideInset * 2);
  const int titleH = measureWrappedHeight(renderer, kTitleFontId, textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  const int authorH =
      author ? measureWrappedHeight(renderer, kAuthorFontId, textMaxW, author, kAuthorMaxLines, EpdFontFamily::REGULAR)
             : 0;
  const int textBlockH = titleH + (author ? (kTitleAuthorGap + authorH) : 0);
  // Under-cover text reserve (gap + title/author). Group must fit in the band.
  const int underCoverH = kMinGapCoverToText + textBlockH;

  // Bare plate = gen plate (420×560) when it fits — 1:1 multipass, no scale.
  // Shrink only if the band cannot hold cover + title/author.
  const int maxCoverH = std::max(160, bandH - underCoverH);
  const int maxCoverW = std::max(120, pageW - kSideInset * 2);
  constexpr int kAspectW = BareMetrics::homeCoverImageWidth;
  constexpr int kAspectH = BareMetrics::homeCoverImageHeight;
  int coverW = kAspectW;
  int coverH = kAspectH;
  if (coverW > maxCoverW || coverH > maxCoverH) {
    coverW = maxCoverW;
    coverH = std::max(1, (coverW * kAspectH + kAspectW / 2) / kAspectW);
    if (coverH > maxCoverH) {
      coverH = maxCoverH;
      coverW = std::max(1, (coverH * kAspectW + kAspectH / 2) / kAspectW);
      if (coverW > maxCoverW) coverW = maxCoverW;
    }
  }

  // Center the whole group (cover → title → author) in the free band.
  const int groupH = coverH + underCoverH;
  const int groupTop = bandTop + std::max(0, (bandH - groupH) / 2);
  const int coverX = (pageW - coverW) / 2;
  const int coverY = groupTop;
  const Rect coverRect{coverX, coverY, coverW, coverH};

  // Snapshot the layout plate (coverRect). Multipass centers the on-disk thumb
  // inside this slot (Bare). Storing a smaller art-only rect made heap/snapshot
  // and multipass positioning brittle after gen.
  if (!coverRendered || !bufferRestored) {
    drawCoverImage(renderer, coverRect, book);
    coverBufferStored = storeCoverBuffer(coverRect.x, coverRect.y, coverRect.width, coverRect.height);
    coverRendered = true;
  }

  // Title directly under the cover; author tight under title.
  const int textTop = coverY + coverH + kMinGapCoverToText;
  int textY = textTop;
  textY = drawCenteredWrapped(renderer, kTitleFontId, pageW / 2, textY, textMaxW, title, kTitleMaxLines,
                              EpdFontFamily::BOLD);
  if (author) {
    textY += kTitleAuthorGap;
    drawCenteredWrapped(renderer, kAuthorFontId, pageW / 2, textY, textMaxW, author, kAuthorMaxLines,
                        EpdFontFamily::REGULAR);
  }
}
