#include "TxtReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const int currentPage, const int totalPages,
                                             const int bookProgressPercent, const uint8_t currentOrientation,
                                             const uint8_t currentPageTurnOption, const uint8_t currentPageJumpOption)
    : Activity("TxtReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems()),
      title(title),
      pendingOrientation(currentOrientation),
      selectedPageTurnOption(currentPageTurnOption),
      selectedPageJumpOption(currentPageJumpOption),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<TxtReaderMenuActivity::MenuItem> TxtReaderMenuActivity::buildMenuItems() {
  std::vector<MenuItem> items;
  items.reserve(6);
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::PAGE_JUMP_STEP, StrId::STR_PAGE_JUMP_STEP});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

void TxtReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void TxtReaderMenuActivity::onExit() { Activity::onExit(); }

void TxtReaderMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;

    // Cycle inline-value options without exiting the menu.
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }
    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }
    if (selectedAction == MenuAction::PAGE_JUMP_STEP) {
      selectedPageJumpOption = (selectedPageJumpOption + 1) % pageJumpLabels.size();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption,
                         selectedPageJumpOption});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption, selectedPageJumpOption};
    setResult(std::move(result));
    finish();
    return;
  }
}

void TxtReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Progress summary (page N/M plus book %) — centered within the content area
  // so the text respects the same landscape/inverted hint gutters as the title.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine =
        std::to_string(currentPage) + "/" + std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const int progressX = contentX + (contentWidth - renderer.getTextWidth(UI_10_FONT_ID, progressLine.c_str())) / 2;
  renderer.drawText(UI_10_FONT_ID, progressX, 45 + contentY, progressLine.c_str());

  const int startY = 75 + contentY;
  constexpr int lineHeight = 30;

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY = startY + (i * lineHeight);
    const bool isSelected = (static_cast<int>(i) == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, I18N.get(menuItems[i].labelId), !isSelected);

    const char* value = nullptr;
    if (menuItems[i].action == MenuAction::ROTATE_SCREEN) {
      value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (menuItems[i].action == MenuAction::AUTO_PAGE_TURN) {
      value = pageTurnLabels[selectedPageTurnOption];
    } else if (menuItems[i].action == MenuAction::PAGE_JUMP_STEP) {
      value = pageJumpLabels[selectedPageJumpOption];
    }
    if (value) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
