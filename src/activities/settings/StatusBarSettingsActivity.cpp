#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 9;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_STATUS_ITEMS_POSITION,
                                     StrId::STR_CHAPTER_PAGE_COUNT,
                                     StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                     StrId::STR_TITLE,
                                     StrId::STR_BATTERY,
                                     StrId::STR_UPPER_PROGRESS_BAR,
                                     StrId::STR_UPPER_PROGRESS_BAR_THICKNESS,
                                     StrId::STR_LOWER_PROGRESS_BAR,
                                     StrId::STR_LOWER_PROGRESS_BAR_THICKNESS};
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int STATUS_ITEMS_POSITION_ITEMS = 2;
const StrId statusItemsPositionNames[STATUS_ITEMS_POSITION_ITEMS] = {StrId::STR_TOP, StrId::STR_BOTTOM};

constexpr int previewHorizontalInset = 10;
constexpr int previewHeight = 78;
constexpr int previewInnerMargin = 4;

void drawPreviewProgressBar(const GfxRenderer& renderer, const Rect& rect, const uint8_t progressBar,
                            const uint8_t thickness, const bool topEdge) {
  if (progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return;
  }

  const int percent = progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS ? 75 : 25;
  const int barHeight = (thickness + 1) * 2;
  const int y = topEdge ? rect.y + previewInnerMargin : rect.y + rect.height - previewInnerMargin - barHeight;
  const int barWidth = (rect.width - previewInnerMargin * 2) * percent / 100;
  renderer.fillRect(rect.x + previewInnerMargin, y, barWidth, barHeight);
}

void drawPreviewStatusItems(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  const bool hasProgressText = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage;
  const bool hasTitle = SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  const bool hasStatusItems = hasProgressText || hasTitle || SETTINGS.statusBarBattery;
  if (!hasStatusItems) {
    return;
  }

  const bool statusItemsAtTop =
      SETTINGS.statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int adjacentProgressHeight = statusItemsAtTop
                                         ? UITheme::getProgressBarHeight(SETTINGS.statusBarUpperProgressBar,
                                                                         SETTINGS.statusBarUpperProgressBarThickness)
                                         : UITheme::getProgressBarHeight(SETTINGS.statusBarLowerProgressBar,
                                                                         SETTINGS.statusBarLowerProgressBarThickness);
  const int statusItemsHeight = UITheme::getStatusBarItemsHeight();
  const int textY = statusItemsAtTop
                        ? rect.y + previewInnerMargin + adjacentProgressHeight + 4
                        : rect.y + rect.height - previewInnerMargin - adjacentProgressHeight - statusItemsHeight + 4;

  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(
        renderer, Rect{rect.x + previewInnerMargin + 2, textY, metrics.batteryWidth, metrics.batteryHeight}, false);
  }

  int progressTextWidth = 0;
  if (hasProgressText) {
    char progressStr[32] = "";
    if (SETTINGS.statusBarChapterPageCount && SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %d%%", 8, 32, 75);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d%%", 75);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", 8, 32);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - previewInnerMargin - 2 - progressTextWidth, textY,
                      progressStr);
  }

  if (!hasTitle) {
    return;
  }

  const char* title = SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE
                          ? tr(STR_EXAMPLE_BOOK)
                          : tr(STR_EXAMPLE_CHAPTER);
  const int leftReserve = SETTINGS.statusBarBattery ? metrics.batteryWidth + 28 : 6;
  const int rightReserve = progressTextWidth > 0 ? progressTextWidth + 18 : 6;
  const int titleAreaWidth = rect.width - previewInnerMargin * 2 - leftReserve - rightReserve;
  if (titleAreaWidth <= 0) {
    return;
  }

  std::string previewTitle = renderer.truncatedText(SMALL_FONT_ID, title, titleAreaWidth);
  const int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, previewTitle.c_str());
  renderer.drawText(SMALL_FONT_ID, rect.x + previewInnerMargin + leftReserve + (titleAreaWidth - titleWidth) / 2, textY,
                    previewTitle.c_str());
}
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  // Clamp status bar settings in case of corrupt/migrated data.
  if (SETTINGS.statusBarUpperProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarUpperProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarLowerProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarLowerProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarUpperProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarUpperProgressBarThickness =
        CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarLowerProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarLowerProgressBarThickness =
        CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarItemsPosition >= STATUS_ITEMS_POSITION_ITEMS) {
    SETTINGS.statusBarItemsPosition = CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
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
    // Status Items Position
    SETTINGS.statusBarItemsPosition = (SETTINGS.statusBarItemsPosition + 1) % STATUS_ITEMS_POSITION_ITEMS;
  } else if (selectedIndex == 1) {
    // Chapter Page Count
    SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
  } else if (selectedIndex == 2) {
    // Book Progress %
    SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
  } else if (selectedIndex == 3) {
    // Chapter Title
    SETTINGS.statusBarTitle = (SETTINGS.statusBarTitle + 1) % TITLE_ITEMS;
  } else if (selectedIndex == 4) {
    // Show Battery
    SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
  } else if (selectedIndex == 5) {
    // Upper Progress Bar
    SETTINGS.statusBarUpperProgressBar = (SETTINGS.statusBarUpperProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (selectedIndex == 6) {
    // Upper Progress Bar Thickness
    SETTINGS.statusBarUpperProgressBarThickness =
        (SETTINGS.statusBarUpperProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  } else if (selectedIndex == 7) {
    // Lower Progress Bar
    SETTINGS.statusBarLowerProgressBar = (SETTINGS.statusBarLowerProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (selectedIndex == 8) {
    // Lower Progress Bar Thickness
    SETTINGS.statusBarLowerProgressBarThickness =
        (SETTINGS.statusBarLowerProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  }
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_STATUS_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewLabelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int previewAreaHeight = previewLabelHeight + previewHeight + metrics.verticalSpacing * 2;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - previewAreaHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          return I18N.get(statusItemsPositionNames[SETTINGS.statusBarItemsPosition]);
        } else if (index == 1) {
          return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 2) {
          return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 3) {
          return I18N.get(titleNames[SETTINGS.statusBarTitle]);
        } else if (index == 4) {
          return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 5) {
          return I18N.get(progressBarNames[SETTINGS.statusBarUpperProgressBar]);
        } else if (index == 6) {
          return I18N.get(progressBarThicknessNames[SETTINGS.statusBarUpperProgressBarThickness]);
        } else if (index == 7) {
          return I18N.get(progressBarNames[SETTINGS.statusBarLowerProgressBar]);
        } else if (index == 8) {
          return I18N.get(progressBarThicknessNames[SETTINGS.statusBarLowerProgressBarThickness]);
        } else {
          return tr(STR_HIDE);
        }
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int previewLabelY = contentTop + contentHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, previewLabelY, tr(STR_PREVIEW));
  const Rect previewRect{previewHorizontalInset, previewLabelY + previewLabelHeight + metrics.verticalSpacing / 2,
                         pageWidth - previewHorizontalInset * 2, previewHeight};
  renderer.drawRect(previewRect.x, previewRect.y, previewRect.width, previewRect.height);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarUpperProgressBar,
                         SETTINGS.statusBarUpperProgressBarThickness, true);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarLowerProgressBar,
                         SETTINGS.statusBarLowerProgressBarThickness, false);
  drawPreviewStatusItems(renderer, previewRect, metrics);

  renderer.displayBuffer();
}
