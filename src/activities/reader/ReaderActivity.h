#pragma once
#include <memory>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

  // One-shot open hints set by Home (or others) before goToReader(); consumed by EpubReader.
  static bool s_preferFastFirstRefresh;
  static bool s_deferFirstPageTextAa;
  static uint32_t s_openWallStartMs;

 public:
  // Call immediately before activityManager.goToReader().
  // preferFastFirstRefresh: home greys already settled → first page FAST instead of HALF.
  // deferFirstPageTextAa: first ink is BW only; AA catch-up render follows.
  static void setOpenHints(bool preferFastFirstRefresh, bool deferFirstPageTextAa);
  // Returns true if hints were pending (always clears). openWallStartMs is millis() at setOpenHints.
  static bool takeOpenHints(bool& preferFastFirstRefresh, bool& deferFirstPageTextAa, uint32_t& openWallStartMs);
  static uint32_t openWallStartMs() { return s_openWallStartMs; }

  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
