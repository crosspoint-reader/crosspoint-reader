#pragma once
#include <I18n.h>
#include <Xtc.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction { SELECT_CHAPTER, AUTO_PAGE_TURN, GO_TO_PAGE, GO_HOME, DELETE_CACHE };

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int currentChapter,
                                 int totalChapters, uint32_t currentPage, uint32_t totalPages);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems();

  const std::vector<MenuItem> menuItems;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentChapter = 0;
  int totalChapters = 0;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
};
