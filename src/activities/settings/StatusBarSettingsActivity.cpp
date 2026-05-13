#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Base menu items (without clock). Clock is appended conditionally at index 5.
constexpr int MENU_ITEMS_NO_CLOCK = 9;
constexpr int MENU_ITEMS_WITH_CLOCK = 10;
const StrId menuNames[MENU_ITEMS_WITH_CLOCK] = {StrId::STR_STATUS_ITEMS_POSITION,
                                                StrId::STR_CHAPTER_PAGE_COUNT,
                                                StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                                StrId::STR_TITLE,
                                                StrId::STR_BATTERY,
                                                StrId::STR_CLOCK,
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
  const int barHeight = UITheme::getProgressBarHeight(progressBar, thickness);
  const int y = topEdge ? rect.y + previewInnerMargin : rect.y + rect.height - previewInnerMargin - barHeight;
  const int barWidth = (rect.width - previewInnerMargin * 2) * percent / 100;
  renderer.fillRect(rect.x + previewInnerMargin, y, barWidth, barHeight);
}

void drawPreviewStatusItems(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  const bool hasProgressText = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage;
  const bool hasTitle = SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  const bool hasStatusItems =
      hasProgressText || hasTitle || SETTINGS.statusBarBattery || (SETTINGS.useClock && SETTINGS.statusBarClock);
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

  const bool showBatteryPercentage =
      SETTINGS.statusBarBattery &&
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  const bool showClock = SETTINGS.useClock && SETTINGS.statusBarClock;
  const int previewClockWidth = showClock ? renderer.getTextWidth(SMALL_FONT_ID, "00:00") : 0;

  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{rect.x + previewInnerMargin + 2, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
  }
  if (showClock) {
    const int clockX = rect.x + previewInnerMargin + (SETTINGS.statusBarBattery ? metrics.batteryWidth + 8 : 0);
    renderer.drawText(SMALL_FONT_ID, clockX, textY, "00:00");
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
  int leftReserve = 6;
  if (SETTINGS.statusBarBattery) {
    leftReserve = metrics.batteryWidth + 28;
    if (showBatteryPercentage) {
      const int percentReserve = 2 + metrics.batteryWidth + BaseTheme::batteryPercentSpacing +
                                 renderer.getTextWidth(SMALL_FONT_ID, "100%") + 8;
      leftReserve = std::max(leftReserve, percentReserve);
    }
    if (showClock) {
      leftReserve += previewClockWidth + 8;
    }
  } else if (showClock) {
    leftReserve = std::max(leftReserve, previewClockWidth + 8);
  }
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

  const int menuCount = SETTINGS.useClock ? MENU_ITEMS_WITH_CLOCK : MENU_ITEMS_NO_CLOCK;
  if (selectedIndex >= menuCount) {
    selectedIndex = 0;
  }

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
  const int menuCount = SETTINGS.useClock ? MENU_ITEMS_WITH_CLOCK : MENU_ITEMS_NO_CLOCK;
  buttonNavigator.onNextList(selectedIndex, menuCount, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, menuCount, [this] { requestUpdate(); });
}

void StatusBarSettingsActivity::handleSelection() {
  // When clock is hidden, indices 6+ shift down by 1 (clock slot at index 5 is absent).
  // We remap the logical index to match the menuNames array which always includes clock at slot 5.
  const int logicalIndex = (!SETTINGS.useClock && selectedIndex >= 5) ? selectedIndex + 1 : selectedIndex;

  if (logicalIndex == 0) {
    // Status Items Position
    SETTINGS.statusBarItemsPosition = (SETTINGS.statusBarItemsPosition + 1) % STATUS_ITEMS_POSITION_ITEMS;
  } else if (logicalIndex == 1) {
    // Chapter Page Count
    SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
  } else if (logicalIndex == 2) {
    // Book Progress %
    SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
  } else if (logicalIndex == 3) {
    // Title
    SETTINGS.statusBarTitle = (SETTINGS.statusBarTitle + 1) % TITLE_ITEMS;
  } else if (logicalIndex == 4) {
    // Battery
    SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
  } else if (logicalIndex == 5 && SETTINGS.useClock) {
    // Clock
    SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % 2;
  } else if (logicalIndex == 6) {
    // Upper Progress Bar
    SETTINGS.statusBarUpperProgressBar = (SETTINGS.statusBarUpperProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (logicalIndex == 7) {
    // Upper Progress Bar Thickness
    SETTINGS.statusBarUpperProgressBarThickness =
        (SETTINGS.statusBarUpperProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  } else if (logicalIndex == 8) {
    // Lower Progress Bar
    SETTINGS.statusBarLowerProgressBar = (SETTINGS.statusBarLowerProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (logicalIndex == 9) {
    // Lower Progress Bar Thickness
    SETTINGS.statusBarLowerProgressBarThickness =
        (SETTINGS.statusBarLowerProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  }
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int pageWidth = (int)renderer.getScreenWidth();
  const int pageHeight = (int)renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_CUSTOMISE_STATUS_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewLabelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int previewAreaHeight = previewLabelHeight + previewHeight + metrics.verticalSpacing * 2;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - previewAreaHeight - metrics.verticalSpacing * 2;
  const int menuCount = SETTINGS.useClock ? MENU_ITEMS_WITH_CLOCK : MENU_ITEMS_NO_CLOCK;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuCount),
      static_cast<int>(selectedIndex),
      [](int index) {
        const int logicalIndex = (!SETTINGS.useClock && index >= 5) ? index + 1 : index;
        return std::string(I18N.get(menuNames[logicalIndex]));
      },
      nullptr, nullptr,
      [this](int index) {
        // When clock is hidden, indices 6+ shift down by 1 in the display list but we still
        // index into menuNames which has clock at slot 5. Remap for value display.
        const int logicalIndex = (!SETTINGS.useClock && index >= 5) ? index + 1 : index;

        if (logicalIndex == 0) {
          return I18N.get(statusItemsPositionNames[SETTINGS.statusBarItemsPosition]);
        } else if (logicalIndex == 1) {
          return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (logicalIndex == 2) {
          return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (logicalIndex == 3) {
          return I18N.get(titleNames[SETTINGS.statusBarTitle]);
        } else if (logicalIndex == 4) {
          return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (logicalIndex == 5) {
          return SETTINGS.statusBarClock ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (logicalIndex == 6) {
          return I18N.get(progressBarNames[SETTINGS.statusBarUpperProgressBar]);
        } else if (logicalIndex == 7) {
          return I18N.get(progressBarThicknessNames[SETTINGS.statusBarUpperProgressBarThickness]);
        } else if (logicalIndex == 8) {
          return I18N.get(progressBarNames[SETTINGS.statusBarLowerProgressBar]);
        } else if (logicalIndex == 9) {
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
