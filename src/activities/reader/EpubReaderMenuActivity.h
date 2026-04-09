#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    FOOTNOTES,
    EMBEDDED_STYLE,
    IMAGE_RENDERING,
    TEXT_DARKNESS,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    PULL_REMOTE,
    PUSH_LOCAL,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const int8_t initialEmbeddedStyleOverride, const int8_t initialImageRenderingOverride,
                                  const uint8_t initialTextDarkness);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
    bool isSeparator = false;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes);

  std::function<bool(int)> buildSelectablePredicate() const;

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  int8_t pendingEmbeddedStyleOverride = -1;
  int8_t pendingImageRenderingOverride = -1;
  uint8_t pendingTextDarkness = 1;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<StrId> textDarknessLabels = {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK,
                                                 StrId::STR_MAX_DARK};
  const std::vector<StrId> imageRenderingLabels = {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER,
                                                   StrId::STR_IMAGES_SUPPRESS};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
