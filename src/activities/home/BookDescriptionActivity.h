#pragma once

#include <EpdFontFamily.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

// Scrollable book synopsis (Calibre / OPF dc:description).
// Builtin UI font (not reader/SD) so open is not multi-second glyph loads.
// Page-jumps with a right-edge scrollbar. Basic HTML (<p>, <br>, <b>/<i>).
//
// From menus: bookPath + empty body → paint Loading first, then load + layout.
class BookDescriptionActivity final : public Activity {
 public:
  // body preloaded, or empty + bookPath for deferred load after first paint.
  BookDescriptionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::string body,
                          std::string bookPath = {})
      : Activity("BookDescription", renderer, mappedInput),
        title(std::move(title)),
        body(std::move(body)),
        bookPath(std::move(bookPath)),
        pendingBodyLoad(this->body.empty() && !this->bookPath.empty()),
        linesDirty(!this->body.empty()) {}

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
  // Compact book title under top chrome (1–2 lines, body starts below with gap).
  void layoutTitleBlock(int pageWidth);
  void drawTitleBlock(int pageWidth) const;

  std::string title;
  std::string body;
  std::string bookPath;
  // Menu path: load after Loading frame is on panel (set true only from render).
  bool pendingBodyLoad = false;
  bool loadingFramePainted = false;
  bool linesDirty = true;
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
