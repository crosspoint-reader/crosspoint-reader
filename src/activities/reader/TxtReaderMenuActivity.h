#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class TxtReaderMenuActivity final : public Activity {
 public:
  // Reader-side menu actions for TXT files. A subset of the EPUB menu — TXT
  // has no chapters, footnotes, or sync targets, so we expose only the
  // operations that make sense against a flat byte-offset reader.
  enum class MenuAction {
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    PAGE_JUMP_STEP,
    ROTATE_SCREEN,
    SCREENSHOT,
    GO_HOME,
  };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                 int currentPage, int totalPages, int bookProgressPercent, uint8_t currentOrientation,
                                 uint8_t currentPageTurnOption, uint8_t currentPageJumpOption);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

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
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  uint8_t selectedPageJumpOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  // Auto-turn rate options match the EPUB reader to keep behavior consistent.
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  // Long-press multi-page jump options. Index 0 = OFF (single-page navigation
  // unchanged); other entries become the jump step when the navigation button
  // is held >= 1 second.
  const std::vector<const char*> pageJumpLabels = {I18N.get(StrId::STR_STATE_OFF), "10", "20", "50", "100"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
