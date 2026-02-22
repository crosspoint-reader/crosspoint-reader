#pragma once
#include <Epub.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction { SELECT_CHAPTER, GO_TO_PERCENT, ROTATE_SCREEN, SCREENSHOT, GO_HOME, SYNC, DELETE_CACHE };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const std::function<void(uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        title(title),
        pendingOrientation(currentOrientation),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
        onBack(onBack),
        onAction(onAction) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  // Fixed menu layout (order matters for up/down navigation).
  const std::vector<MenuItem> menuItems = {
      {MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER}, {MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION},
      {MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT},   {MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON},
      {MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON},        {MenuAction::SYNC, StrId::STR_SYNC_PROGRESS},
      {MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE}};
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::function<void(uint8_t)> onBack;
  const std::function<void(MenuAction)> onAction;
};
