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
const StrId timeZoneNames[CrossPointSettings::TIMEZONE_COUNT] = {
    StrId::STR_TZ_UTC,       StrId::STR_TZ_CET,  StrId::STR_TZ_EET,       StrId::STR_TZ_MSK,
    StrId::STR_TZ_UTC_PLUS4, StrId::STR_TZ_IST,  StrId::STR_TZ_UTC_PLUS7, StrId::STR_TZ_UTC_PLUS8,
    StrId::STR_TZ_UTC_PLUS9, StrId::STR_TZ_AEST, StrId::STR_TZ_NZST,      StrId::STR_TZ_UTC_MINUS3,
    StrId::STR_TZ_EST,       StrId::STR_TZ_CST,  StrId::STR_TZ_MST,       StrId::STR_TZ_PST};
}  // namespace

std::vector<ClockSettingsActivity::MenuItem> ClockSettingsActivity::buildMenuItems() {
  std::vector<MenuItem> items;
  items.reserve(7);
  // Settings
  items.push_back({Action::NONE, StrId::STR_SETTINGS_TITLE, true});
  items.push_back({Action::USE_CLOCK, StrId::STR_USE_CLOCK});
  items.push_back({Action::CLOCK_FORMAT, StrId::STR_CLOCK_FORMAT});
  items.push_back({Action::TIMEZONE, StrId::STR_TIMEZONE});

  // Tools
  items.push_back({Action::NONE, StrId::STR_READER_TOOLS, true});
  items.push_back({Action::DETECT_TIMEZONE, StrId::STR_DETECT_TIMEZONE});
  items.push_back({Action::SYNC_TIME, StrId::STR_SYNC_TIME});
  return items;
}

std::function<bool(int)> ClockSettingsActivity::buildSelectablePredicate() const {
  return [this](int index) {
    return index >= 0 && index < static_cast<int>(menuItems.size()) && !menuItems[index].isSeparator;
  };
}

void ClockSettingsActivity::onEnter() {
  Activity::onEnter();
  buttonNavigator.setSelectablePredicate(buildSelectablePredicate(), static_cast<int>(menuItems.size()));
  if (!buildSelectablePredicate()(selectedIndex)) {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
  }
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
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });
}

void ClockSettingsActivity::handleSelection() {
  const auto action = menuItems[selectedIndex].action;
  if (action == Action::USE_CLOCK) {
    SETTINGS.useClock = (SETTINGS.useClock + 1) % 2;
    if (!SETTINGS.useClock) {
      SETTINGS.statusBarClock = 0;
    }
    SETTINGS.saveToFile();
  } else if (action == Action::CLOCK_FORMAT) {
    SETTINGS.clockFormat12h = (SETTINGS.clockFormat12h + 1) % 2;
    SETTINGS.saveToFile();
  } else if (action == Action::TIMEZONE) {
    SETTINGS.timeZone = (SETTINGS.timeZone + 1) % CrossPointSettings::TIMEZONE_COUNT;
    HalClock::applyTimezone(SETTINGS.timeZone);
    SETTINGS.saveToFile();
  } else if (action == Action::SYNC_TIME) {
    auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };
    startActivityForResult(std::make_unique<SyncTimeActivity>(renderer, mappedInput), resultHandler);
  } else if (action == Action::DETECT_TIMEZONE) {
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
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuItems.size()), selectedIndex,
      [this](int index) {
        const auto title = I18N.get(menuItems[index].labelId);
        return menuItems[index].isSeparator ? UITheme::makeSeparatorTitle(title) : title;
      },
      nullptr, nullptr,
      [this](int index) {
        const auto action = menuItems[index].action;
        switch (action) {
          case Action::USE_CLOCK:
            return std::string(SETTINGS.useClock ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
          case Action::CLOCK_FORMAT:
            return std::string(SETTINGS.clockFormat12h ? tr(STR_12H) : tr(STR_24H));
          case Action::TIMEZONE: {
            const auto tzIndex = static_cast<size_t>(SETTINGS.timeZone);
            if (tzIndex < (sizeof(timeZoneNames) / sizeof(timeZoneNames[0]))) {
              return std::string(I18N.get(timeZoneNames[tzIndex]));
            }
            return std::string(tr(STR_TZ_UTC));
          }
          default:
            return std::string("");
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
