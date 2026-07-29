#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Actions available from long-press on a book (recents / dashboard Read / file browser).
// In book mode, only terminal outcomes (Open / Delete / RemoveFromRecents) are returned
// to the parent. Side-effect actions stay inside this menu.
// In custom-items mode (e.g. clipping list), every selection is returned to the parent.
enum class FileBrowserAction : int {
  Open = 0,
  Description = 1,  // Calibre/OPF synopsis (EPUB)
  ResetPace = 2,
  RemoveFromRecents = 3,
  ToggleCompleted = 4,
  Delete = 5,
  DeleteStats = 6,
  DeleteCache = 7,
  ReadingStats = 8,  // Book stats screen (Stats theme long-press)
};

class FileBrowserActionActivity final : public Activity {
 public:
  struct MenuItem {
    FileBrowserAction action;
    StrId labelId;
  };

  // Book long-press menu: path-based items; side effects stay on this activity so the
  // parent (folder / dashboard / recents) is preserved under the stack.
  FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::string bookPath, bool includeRemoveFromRecents, bool openedFromLongPress = false);

  // Custom item list (e.g. clipping actions): selection is returned to the parent as-is.
  FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::vector<MenuItem> items, bool openedFromLongPress = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void rebuildItems();
  void activateSelected();
  void stayInMenu();

  ButtonNavigator buttonNavigator;
  std::string title;
  std::string bookPath;
  bool includeRemoveFromRecents = false;
  bool bookMode = false;  // true: handle side effects; false: return selection to parent
  std::vector<MenuItem> items;
  int selectedIndex = 0;
  bool awaitOpenButtonRelease = false;
};
