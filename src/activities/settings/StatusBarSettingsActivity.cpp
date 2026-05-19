#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Menu items in their natural order. Clock entries are appended only when the
// DS3231 RTC is present so X4 devices don't see them at all.
enum MenuItem {
  ITEM_CHAPTER_PAGE_COUNT = 0,
  ITEM_BOOK_PROGRESS_PERCENTAGE,
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_TITLE,
  ITEM_BATTERY,
  ITEM_XTC_STATUS_BAR,
  ITEM_CLOCK,             // X3 only
  ITEM_CLOCK_FORMAT,      // X3 only
  ITEM_CLOCK_UTC_OFFSET,  // X3 only, launches ClockOffsetActivity
  ITEM_CLOCK_SYNC,        // X3 only, launches ClockSyncActivity
  ITEM_COUNT
};

constexpr int BASE_MENU_ITEMS = ITEM_CLOCK;  // Items shown on every device
constexpr int FULL_MENU_ITEMS = ITEM_COUNT;  // Items shown when RTC is available

const StrId menuNames[FULL_MENU_ITEMS] = {
    StrId::STR_CHAPTER_PAGE_COUNT,
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_TITLE,
    StrId::STR_BATTERY,
    StrId::STR_XTC_STATUS_BAR,
    StrId::STR_CLOCK,
    StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_UTC_OFFSET,
    StrId::STR_CLOCK_SYNC_NOW,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

std::string formatUtcOffset(uint8_t biasedQ) {
  // biasedQ is in quarter-hour steps, biased by 48 (so 48 = UTC+0).
  if (biasedQ > 104) biasedQ = 48;
  int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
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

const int verticalPreviewPadding = 50;
const int verticalPreviewTextPadding = 40;
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  visibleItemCount = halClock.isAvailable() ? FULL_MENU_ITEMS : BASE_MENU_ITEMS;

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

  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;  // Default to UTC+0
  }

  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
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
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_CHAPTER_PAGE_COUNT:
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
      break;
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
      break;
    case ITEM_PROGRESS_BAR:
      SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % PROGRESS_BAR_ITEMS;
      break;
    case ITEM_PROGRESS_BAR_THICKNESS:
      SETTINGS.statusBarProgressBarThickness =
          (SETTINGS.statusBarProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
      break;
    case ITEM_TITLE:
      SETTINGS.statusBarTitle = (SETTINGS.statusBarTitle + 1) % TITLE_ITEMS;
      break;
    case ITEM_BATTERY:
      SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
      break;
    case ITEM_XTC_STATUS_BAR:
      SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % XTC_STATUS_BAR_ITEMS;
      break;
    case ITEM_CLOCK:
      SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % 2;
      break;
    case ITEM_CLOCK_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      break;
    case ITEM_CLOCK_UTC_OFFSET:
      // Launch the dedicated offset picker. It saves on exit, no result handler needed.
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_CLOCK_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
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
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleItemCount, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_CHAPTER_PAGE_COUNT:
            return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_BOOK_PROGRESS_PERCENTAGE:
            return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_PROGRESS_BAR:
            return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
          case ITEM_PROGRESS_BAR_THICKNESS:
            return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
          case ITEM_TITLE:
            return I18N.get(titleNames[SETTINGS.statusBarTitle]);
          case ITEM_BATTERY:
            return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_XTC_STATUS_BAR:
            return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
          case ITEM_CLOCK:
            return SETTINGS.statusBarClock ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_CLOCK_FORMAT: {
            const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
            return std::string(I18N.get(clockFormatNames[fmt]));
          }
          case ITEM_CLOCK_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_CLOCK_SYNC:
            return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
          default:
            return tr(STR_HIDE);
        }
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
