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

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                 const uint32_t currentPage, const uint32_t totalPages);

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
  std::string title = "Reader Menu";
  uint8_t selectedPageTurnOption = 0;
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
};
