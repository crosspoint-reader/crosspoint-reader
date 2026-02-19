#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/StatusBar.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CHAPTER_PAGE_COUNT, StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                     StrId::STR_PROGRESS_BAR, StrId::STR_CHAPTER_TITLE, StrId::STR_BATTERY};
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

const char* translatedShow = tr(STR_SHOW);
const char* translatedHide = tr(STR_HIDE);
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  // Clamp statusBarProgressBar in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE;
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Chapter Page Count
    SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
  } else if (selectedIndex == 1) {
    // Book Progress %
    SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
  } else if (selectedIndex == 2) {
    // Progress Bar
    SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (selectedIndex == 3) {
    // Chapter Title
    SETTINGS.statusBarChapterTitle = (SETTINGS.statusBarChapterTitle + 1) % 2;
  } else if (selectedIndex == 4) {
    // Show Battery
    SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
  }
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_STATUS_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          return SETTINGS.statusBarChapterPageCount ? translatedShow : translatedHide;
        } else if (index == 1) {
          return SETTINGS.statusBarBookProgressPercentage ? translatedShow : translatedHide;
        } else if (index == 2) {
          return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
        } else if (index == 3) {
          return SETTINGS.statusBarChapterTitle ? translatedShow : translatedHide;
        } else if (index == 4) {
          return SETTINGS.statusBarBattery ? translatedShow : translatedHide;
        } else {
          return translatedHide;
        }
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  int verticalPreviewPadding = 50;

  // Add status bar margin
  const bool showProgressBar = SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE;
  if (SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage || showProgressBar ||
      SETTINGS.statusBarChapterTitle || SETTINGS.statusBarBattery) {
    // Add additional margin for status bar if progress bar is shown
    orientedMarginBottom += 19 - SETTINGS.screenMargin + (showProgressBar ? (metrics.bookProgressBarHeight + 1) : 0);
  }

  StatusBar::renderStatusBar(renderer, orientedMarginRight, orientedMarginBottom, orientedMarginLeft, 75, 8, 32,
                             tr(STR_EXAMPLE_CHAPTER), verticalPreviewPadding);

  renderer.drawText(UI_10_FONT_ID, orientedMarginLeft,
                    renderer.getScreenHeight() - orientedMarginBottom - 50 - 19 - 19 - 4, tr(STR_PREVIEW));

  renderer.displayBuffer();
}
