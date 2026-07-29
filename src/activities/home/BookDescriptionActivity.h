#pragma once

#include <EpdFontFamily.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

// Scrollable book synopsis (Calibre / OPF dc:description).
// Uses the user's reading font + line/paragraph spacing; page-jumps with a
// right-edge scrollbar. Basic HTML from Calibre (<p>, <br>, <b>/<strong>,
// <i>/<em>) is preserved as paragraph gaps and bold/italic runs.
class BookDescriptionActivity final : public Activity {
 public:
  BookDescriptionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                          std::string body)
      : Activity("BookDescription", renderer, mappedInput),
        title(std::move(title)),
        body(std::move(body)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Public so layout helpers in the .cpp can build styled runs.
  struct Run {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };
  struct Line {
    std::vector<Run> runs;
    bool isParagraphGap = false;  // empty row for paragraph spacing
  };

 private:
  void rebuildLines();
  void layoutFromHtml(const std::string& html);
  void wrapParagraph(const std::vector<Run>& runs);
  void drawScrollBar(int totalLines) const;
  int pageStep() const;
  int rowHeight(const Line& line) const;
  // Large multi-line book title under top chrome (fills most of the width).
  void layoutTitleBlock(int pageWidth);
  void drawTitleBlock(int pageWidth) const;

  std::string title;
  std::string body;
  std::vector<Line> lines;
  std::vector<std::string> titleLines;
  int titleFontId = 0;
  int titleBlockBottom = 0;  // y just below the title block (content starts after this)
  int fontId = 0;
  int lineHeight = 1;
  int paragraphGap = 0;
  int scrollLine = 0;
  int visibleLines = 1;
  int contentTop = 0;
  int bodyHeight = 0;
  int textLeft = 0;
  int textWidth = 1;
};
