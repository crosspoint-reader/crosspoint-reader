#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    BLUETOOTH,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    SAVE_CLIPPING,
    VIEW_CLIPPINGS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class Category : uint8_t { NAVIGATION, TOOLS, APPEARANCE, UTILITIES, COUNT };

  struct MenuState {
    Category category = Category::NAVIGATION;
    Category selectedCategory = Category::NAVIGATION;
    MenuAction action = MenuAction::SELECT_CHAPTER;
    bool categorySelected = false;
    bool expanded = true;
  };

  static Category categoryFor(MenuAction action);
  static StrId categoryLabel(Category category);
  static void toggleBluetooth();
  static void buildMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks, bool hasClippings);

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, bool hasBookmarks,
                                  bool hasClippings, MenuState& menuState);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
  bool acceptsRemoteInput() const override { return routingReady(); }

 private:
  static constexpr size_t MAX_MENU_ITEMS = 19;
  static constexpr size_t CATEGORY_COUNT = static_cast<size_t>(Category::COUNT);
  static constexpr size_t MAX_VISIBLE_ROWS = MAX_MENU_ITEMS + CATEGORY_COUNT;
  struct VisibleRow {
    Category category;
    int menuIndex;  // -1 identifies the expandable category row.
  };
  freeink::ui::ListItem menuRowItems[MAX_VISIBLE_ROWS]{};
  VisibleRow visibleRows[MAX_VISIBLE_ROWS]{};
  int visibleRowCount = 0;
  // Borrowed from the retained reader; remembered only for this open book.
  MenuState& menuState;
  void rebuildRows();
  void rememberSelection();
  void switchCategory(int direction);
  void navigateButtons() override;
  void drawFooter() override;

  int listCount() const override { return visibleRowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back and Confirm act on release; navigation belongs to ButtonNavigator.
  bool handleButtons() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;

  void closeCancelled();

  // Fixed menu layout
  std::vector<MenuItem> menuItems;

  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
