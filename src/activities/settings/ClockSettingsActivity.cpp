#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "DetectTimezoneActivity.h"
#include "MappedInputManager.h"
#include "SyncTimeActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USE_CLOCK, StrId::STR_CLOCK_FORMAT, StrId::STR_TIMEZONE,
                                     StrId::STR_SYNC_TIME, StrId::STR_DETECT_TIMEZONE};

const StrId timeZoneNames[CrossPointSettings::TIMEZONE_COUNT] = {
    StrId::STR_TZ_UTC,       StrId::STR_TZ_CET,  StrId::STR_TZ_EET,       StrId::STR_TZ_MSK,
    StrId::STR_TZ_UTC_PLUS4, StrId::STR_TZ_IST,  StrId::STR_TZ_UTC_PLUS7, StrId::STR_TZ_UTC_PLUS8,
    StrId::STR_TZ_UTC_PLUS9, StrId::STR_TZ_AEST, StrId::STR_TZ_NZST,      StrId::STR_TZ_UTC_MINUS3,
    StrId::STR_TZ_EST,       StrId::STR_TZ_CST,  StrId::STR_TZ_MST,       StrId::STR_TZ_PST};
}  // namespace

void ClockSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ClockSettingsActivity::onExit() { Activity::onExit(); }

void ClockSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

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

void ClockSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    SETTINGS.useClock = (SETTINGS.useClock + 1) % 2;
    if (!SETTINGS.useClock) {
      SETTINGS.statusBarClock = 0;
    }
    SETTINGS.saveToFile();
  } else if (selectedIndex == 1) {
    SETTINGS.clockFormat12h = (SETTINGS.clockFormat12h + 1) % 2;
    SETTINGS.saveToFile();
  } else if (selectedIndex == 2) {
    SETTINGS.timeZone = (SETTINGS.timeZone + 1) % CrossPointSettings::TIMEZONE_COUNT;
    HalClock::applyTimezone(SETTINGS.timeZone);
    SETTINGS.saveToFile();
  } else if (selectedIndex == 3) {
    auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };
    startActivityForResult(std::make_unique<SyncTimeActivity>(renderer, mappedInput), resultHandler);
  } else if (selectedIndex == 4) {
    auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };
    startActivityForResult(std::make_unique<DetectTimezoneActivity>(renderer, mappedInput), resultHandler);
  }
}

void ClockSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SETTINGS));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_CLOCK_SETTINGS_WARNING));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) {
        if (index == 0) {
          return std::string(SETTINGS.useClock ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (index == 1) {
          return std::string(SETTINGS.clockFormat12h ? tr(STR_12H) : tr(STR_24H));
        }
        if (index == 2) {
          const auto tzIndex = static_cast<size_t>(SETTINGS.timeZone);
          if (tzIndex < (sizeof(timeZoneNames) / sizeof(timeZoneNames[0]))) {
            return std::string(I18N.get(timeZoneNames[tzIndex]));
          }
          return std::string(tr(STR_TZ_UTC));
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
