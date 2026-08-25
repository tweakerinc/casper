#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PngToBmpConverter.h>
#include <Txt.h>
#include <Xtc.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "util/CrossPointPaths.h"

namespace {
// Temp 2-bit BMP written when painting a PNG sleep image (reuses BMP greyscale path).
constexpr const char* kSleepPngTempBmp = "/.crosspoint/sleep_from_png.bmp";

bool isSleepImageName(const std::string& filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Wallpaper only (Dark/Light/Cover/Custom/Blank). No last-frame moon, no
  // "Going to sleep" popup — those were extra full refreshes before the art.
  // Suspend UI Dark Mode invert so Sleep Screen polarity is not double-inverted.
  const bool uiDark = renderer.getInvertOnDisplay();
  renderer.setInvertOnDisplay(false);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      renderBlankSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      renderCustomSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      renderCoverSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        renderCoverSleepScreen();
      } else {
        renderCustomSleepScreen();
      }
      break;
    default:
      renderDefaultSleepScreen();
      break;
  }
  renderer.setInvertOnDisplay(uiDark);
}

void SleepActivity::renderCustomSleepScreen() const {
  // Wallpapers: /.sleep only, with /sleep as fallback if /.sleep is missing.
  // (No root /sleep.bmp|/sleep.png — keep everything in the folder.)
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    if (dir) dir.close();
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // Collect BMP + PNG from the sleep folder (same pool, random pick).
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      if (!isSleepImageName(filename)) {
        LOG_DBG("SLP", "Skipping non-image sleep file: %s", name);
        dirFile.close();
        continue;
      }
      if (FsHelpers::hasBmpExtension(filename)) {
        Bitmap bitmap(dirFile);
        if (bitmap.parseHeaders() != BmpReaderError::Ok) {
          LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
          dirFile.close();
          continue;
        }
      }
      // PNG validated at decode time (header parse alone needs a full open path).
      files.emplace_back(filename);
      dirFile.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      LOG_DBG("SLP", "Randomly loading: %s", filename.c_str());
      delay(100);
      if (FsHelpers::hasPngExtension(files[randomFileIndex])) {
        if (renderPngSleepScreen(filename)) {
          dir.close();
          return;
        }
      } else {
        HalFile randFile;
        if (Storage.openFileForRead("SLP", filename, randFile)) {
          Bitmap bitmap(randFile, true);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            renderBitmapSleepScreen(bitmap);
            randFile.close();
            dir.close();
            return;
          }
          randFile.close();
        }
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

bool SleepActivity::renderPngSleepScreen(const std::string& pngPath) const {
  // Convert PNG → temp BMP on SD, then reuse the greyscale multipass BMP path
  // (same crop/filter/HALF settings as sleep.bmp). Sleep has the whole heap free.
  constexpr size_t kMinFree = 48 * 1024;
  if (ESP.getFreeHeap() < kMinFree) {
    LOG_ERR("SLP", "Not enough heap for PNG sleep image free=%u: %s", static_cast<unsigned>(ESP.getFreeHeap()),
            pngPath.c_str());
    return false;
  }

  Storage.mkdir("/.crosspoint");
  if (Storage.exists(kSleepPngTempBmp)) {
    Storage.remove(kSleepPngTempBmp);
  }

  HalFile pngIn;
  if (!Storage.openFileForRead("SLP", pngPath, pngIn)) {
    LOG_ERR("SLP", "Failed to open PNG sleep image: %s", pngPath.c_str());
    return false;
  }
  HalFile bmpOut;
  if (!Storage.openFileForWrite("SLP", kSleepPngTempBmp, bmpOut)) {
    pngIn.close();
    LOG_ERR("SLP", "Failed to create temp sleep BMP");
    return false;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const bool ok = PngToBmpConverter::pngFileToBmpStreamWithSize(pngIn, bmpOut, pageWidth, pageHeight);
  bmpOut.flush();
  bmpOut.close();
  pngIn.close();
  if (!ok) {
    Storage.remove(kSleepPngTempBmp);
    LOG_ERR("SLP", "PNG→BMP conversion failed: %s", pngPath.c_str());
    return false;
  }

  HalFile bmpIn;
  if (!Storage.openFileForRead("SLP", kSleepPngTempBmp, bmpIn)) {
    Storage.remove(kSleepPngTempBmp);
    return false;
  }
  Bitmap bitmap(bmpIn, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    bmpIn.close();
    Storage.remove(kSleepPngTempBmp);
    LOG_ERR("SLP", "Temp sleep BMP invalid after PNG convert");
    return false;
  }
  LOG_DBG("SLP", "PNG sleep via temp BMP: %s", pngPath.c_str());
  renderBitmapSleepScreen(bitmap);
  bmpIn.close();
  // Keep temp for wake re-seed paths that re-open the last painted BMP; remove next convert.
  return true;
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // CrossPoint: sheet-ghost logo + product name only (no "SLEEPING"). Light = white; Dark = full invert
  // (1.5 has no drawImageInverted — invertScreen flips logo + name together).
  renderer.clearScreen();
  constexpr int kLogoSize = 120;
  const int logoY = pageHeight / 2 - kLogoSize / 2 - 24;
  renderer.drawImage(Logo120, (pageWidth - kLogoSize) / 2, logoY, kLogoSize, kLogoSize);
  renderer.drawCenteredText(UI_12_FONT_ID, logoY + kLogoSize + 12, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::DARK) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, CrossPointPaths::kPackageCacheRoot);
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, CrossPointPaths::kPackageCacheRoot);
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, CrossPointPaths::kPackageCacheRoot);
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
