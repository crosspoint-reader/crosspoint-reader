#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../MenuListActivity.h"

class EpubReaderMenuActivity final : public MenuListActivity {
 public:
  // Menu actions identified by StrId of the menu item.
  // Used by the parent activity to interpret the result.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    FOOTNOTES,
    EMBEDDED_STYLE,
    IMAGE_RENDERING,
    TEXT_DARKNESS,
    GO_TO_PERCENT,
    GO_TO_PRINTED_PAGE,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    PULL_REMOTE,
    PUSH_LOCAL,
    STARRED_PAGES,
    STAR_PAGE,
    MARK_AS_READ,
    DELETE_CACHE,
    RENDER_BENCHMARK,
    READING_STATS,
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const int8_t initialEmbeddedStyleOverride, const int8_t initialImageRenderingOverride,
                                  const int8_t initialFontFamilyOverride,
                                  const std::string& initialSdFontFamilyOverride, const int8_t initialFontSizeOverride,
                                  const uint8_t initialTextDarkness, const bool initialBionicReadingOverride,
                                  const int8_t initialParagraphAlignmentOverride, const bool hasStarredPages,
                                  const bool isCurrentPageStarred, const bool hasPrintedPages);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void buildMenuItems(bool hasFootnotes, bool hasStarredPages, bool hasPrintedPages);

  bool currentPageStarred = false;
  void finishWithAction(MenuAction action);

  // MenuListActivity overrides
  std::string getItemValueString(int index) const override;
  void onActionSelected(int index) override;
  void onBackPressed() override;
  void onSettingToggled(int index) override;
  void toggleCurrentItem() override;
  void openSubmenu(const SettingInfo& submenuEntry);

  // Map from StrId to MenuAction for result passing
  static MenuAction actionForNameId(StrId nameId);
  static MenuAction actionForSettingAction(SettingAction action);

  // Pending state (mutated locally, returned to parent on finish)
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  int8_t pendingEmbeddedStyleOverride = -1;
  int8_t pendingImageRenderingOverride = -1;
  int8_t pendingFontFamilyOverride = -1;
  std::string pendingSdFontFamilyOverride;
  int8_t pendingFontSizeOverride = -1;
  uint8_t pendingTextDarkness = 1;
  bool pendingBionicReading = false;
  int8_t pendingParagraphAlignmentOverride = -1;

  static constexpr const char* pageTurnLabels[] = {"", "1", "3", "6", "12"};

  std::string title = "Reader Menu";
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
