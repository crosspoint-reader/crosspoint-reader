#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum class MenuItem {
  ChapterPageCount,
  BookProgressPercentage,
  ProgressBar,
  ProgressBarThickness,
  Title,
  Battery,
  Clock,
  ClockUtcOffset,
  XtcStatusBar,
};

const std::vector<MenuItem>& menuItems() {
  static const std::vector<MenuItem> items = [] {
    std::vector<MenuItem> v = {MenuItem::ChapterPageCount,
                               MenuItem::BookProgressPercentage,
                               MenuItem::ProgressBar,
                               MenuItem::ProgressBarThickness,
                               MenuItem::Title,
                               MenuItem::Battery};
    if (halClock.isAvailable()) {
      v.push_back(MenuItem::Clock);
      v.push_back(MenuItem::ClockUtcOffset);
    }
    v.push_back(MenuItem::XtcStatusBar);
    return v;
  }();
  return items;
}

StrId menuName(MenuItem item) {
  switch (item) {
    case MenuItem::ChapterPageCount:
      return StrId::STR_CHAPTER_PAGE_COUNT;
    case MenuItem::BookProgressPercentage:
      return StrId::STR_BOOK_PROGRESS_PERCENTAGE;
    case MenuItem::ProgressBar:
      return StrId::STR_PROGRESS_BAR;
    case MenuItem::ProgressBarThickness:
      return StrId::STR_PROGRESS_BAR_THICKNESS;
    case MenuItem::Title:
      return StrId::STR_TITLE;
    case MenuItem::Battery:
      return StrId::STR_BATTERY;
    case MenuItem::Clock:
      return StrId::STR_CLOCK;
    case MenuItem::ClockUtcOffset:
      return StrId::STR_CLOCK_UTC_OFFSET;
    case MenuItem::XtcStatusBar:
      return StrId::STR_XTC_STATUS_BAR;
  }
  return StrId::STR_HIDE;
}

// UTC offset range: 0 = UTC-12:00, 24 = UTC+0, 52 = UTC+14:00 (half-hour steps)
constexpr uint8_t UTC_OFFSET_MIN = 0;
constexpr uint8_t UTC_OFFSET_MAX = 52;

std::string formatUtcOffset(uint8_t biased) {
  int totalMinutes = (static_cast<int>(biased) - 24) * 30;  // -720 to +840
  bool neg = totalMinutes < 0;
  int absMinutes = neg ? -totalMinutes : totalMinutes;
  int hours = absMinutes / 60;
  int mins = absMinutes % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', hours, mins);
  return buf;
}
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int XTC_STATUS_BAR_ITEMS = 3;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};

const int widthMargin = 10;
const int verticalPreviewPadding = 50;
const int verticalPreviewTextPadding = 40;
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  // Clamp statusBarProgressBar and statusBarTitle in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarTitle >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
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
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems().size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems().size()));
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems().size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems().size()));
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  switch (menuItems()[selectedIndex]) {
    case MenuItem::ChapterPageCount:
      // Chapter Page Count
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
      break;
    case MenuItem::BookProgressPercentage:
      // Book Progress %
      SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
      break;
    case MenuItem::ProgressBar:
      // Progress Bar
      SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % PROGRESS_BAR_ITEMS;
      break;
    case MenuItem::ProgressBarThickness:
      // Progress Bar Thickness
      SETTINGS.statusBarProgressBarThickness =
          (SETTINGS.statusBarProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
      break;
    case MenuItem::Title:
      // Chapter Title
      SETTINGS.statusBarTitle = (SETTINGS.statusBarTitle + 1) % TITLE_ITEMS;
      break;
    case MenuItem::Battery:
      // Show Battery
      SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
      break;
    case MenuItem::Clock:
      // Show Clock (X3 only)
      SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % 2;
      break;
    case MenuItem::ClockUtcOffset:
      // UTC Offset (cycle in half-hour steps)
      if (SETTINGS.clockUtcOffset >= UTC_OFFSET_MAX) {
        SETTINGS.clockUtcOffset = UTC_OFFSET_MIN;
      } else {
        SETTINGS.clockUtcOffset++;
      }
      break;
    case MenuItem::XtcStatusBar:
      // XTC Status Bar
      SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % XTC_STATUS_BAR_ITEMS;
      break;
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
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuItems().size()),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuName(menuItems()[index]))); },
      nullptr, nullptr,
      [this](int index) -> std::string {
        // Draw status for each setting
        switch (menuItems()[index]) {
          case MenuItem::ChapterPageCount:
            return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
          case MenuItem::BookProgressPercentage:
            return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
          case MenuItem::ProgressBar:
            return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
          case MenuItem::ProgressBarThickness:
            return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
          case MenuItem::Title:
            return I18N.get(titleNames[SETTINGS.statusBarTitle]);
          case MenuItem::Battery:
            return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
          case MenuItem::Clock:
            return SETTINGS.statusBarClock ? tr(STR_SHOW) : tr(STR_HIDE);
          case MenuItem::ClockUtcOffset:
            return formatUtcOffset(SETTINGS.clockUtcOffset);
          case MenuItem::XtcStatusBar:
            return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
        }
        return tr(STR_HIDE);
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = tr(STR_EXAMPLE_BOOK);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_EXAMPLE_CHAPTER);
  }

  GUI.drawStatusBar(renderer, 75, 8, 32, title, verticalPreviewPadding);

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding,
                    renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() - verticalPreviewPadding -
                        verticalPreviewTextPadding,
                    tr(STR_PREVIEW));

  renderer.displayBuffer();
}
