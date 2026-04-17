#include "WeatherSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WeatherClient.h>
#include <WeatherSettingsStore.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WeatherSettingsActivity::buildMenuItems() {
  menuItems.reserve(10);
  menuItems.push_back(SettingInfo::Separator(StrId::STR_SETTINGS_TITLE));
  menuItems.push_back(SettingInfo::Toggle(StrId::STR_USE_WEATHER, &CrossPointSettings::useWeather, "useWeather",
                                          StrId::STR_CAT_SYSTEM));

  menuItems.push_back(SettingInfo::Separator(StrId::STR_WEATHER_LOCATION));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_LOCATION, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_LATITUDE, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_LONGITUDE, SettingAction::None));

  menuItems.push_back(SettingInfo::Separator(StrId::STR_WEATHER_UNITS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_TEMP_UNIT, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_WIND_UNIT, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_PRECIP_UNIT, SettingAction::None));
}

void WeatherSettingsActivity::onEnter() {
  MenuListActivity::onEnter();
  showingSearchResults = false;
}

void WeatherSettingsActivity::loop() {
  if (showingSearchResults) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      showingSearchResults = false;
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!searchResults.empty() && selectedIndex < static_cast<int>(searchResults.size())) {
        const auto& result = searchResults[selectedIndex];
        WEATHER_SETTINGS.setLocation(result.latitude, result.longitude, result.name + ", " + result.country);
        WEATHER_SETTINGS.saveToFile();
        showingSearchResults = false;
        selectedIndex = 0;
        requestUpdate();
      }
      return;
    }

    buttonNavigator.onNext([this] {
      if (searchResults.empty()) return;
      selectedIndex = (selectedIndex + 1) % static_cast<int>(searchResults.size());
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      if (searchResults.empty()) return;
      const int size = static_cast<int>(searchResults.size());
      selectedIndex = (selectedIndex + size - 1) % size;
      requestUpdate();
    });
    return;
  }

  MenuListActivity::loop();
}

std::string WeatherSettingsActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  switch (item.nameId) {
    case StrId::STR_WEATHER_LOCATION: {
      auto name = WEATHER_SETTINGS.getLocationName();
      return name.empty() ? std::string(tr(STR_NOT_SET)) : name;
    }
    case StrId::STR_WEATHER_LATITUDE: {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLatitude());
      return std::string(buf);
    }
    case StrId::STR_WEATHER_LONGITUDE: {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLongitude());
      return std::string(buf);
    }
    case StrId::STR_WEATHER_TEMP_UNIT:
      return WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
    case StrId::STR_WEATHER_WIND_UNIT:
      switch (WEATHER_SETTINGS.getWindUnit()) {
        case WeatherWindUnit::KMH:
          return "km/h";
        case WeatherWindUnit::MS:
          return "m/s";
        case WeatherWindUnit::MPH:
          return "mph";
        case WeatherWindUnit::KNOTS:
          return "kn";
      }
      return "km/h";
    case StrId::STR_WEATHER_PRECIP_UNIT:
      return WEATHER_SETTINGS.getPrecipUnit() == WeatherPrecipUnit::MM ? "mm" : "in";
    default:
      return MenuListActivity::getItemValueString(index);
  }
}

void WeatherSettingsActivity::onActionSelected(int index) {
  const auto& item = menuItems[index];
  if (item.nameId == StrId::STR_WEATHER_LOCATION) {
    launchCitySearch();
  } else if (item.nameId == StrId::STR_WEATHER_LATITUDE) {
    launchLatitudeEntry();
  } else if (item.nameId == StrId::STR_WEATHER_LONGITUDE) {
    launchLongitudeEntry();
  } else if (item.nameId == StrId::STR_WEATHER_TEMP_UNIT) {
    auto current = WEATHER_SETTINGS.getTempUnit();
    WEATHER_SETTINGS.setTempUnit(current == WeatherTempUnit::CELSIUS ? WeatherTempUnit::FAHRENHEIT
                                                                     : WeatherTempUnit::CELSIUS);
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  } else if (item.nameId == StrId::STR_WEATHER_WIND_UNIT) {
    auto current = static_cast<uint8_t>(WEATHER_SETTINGS.getWindUnit());
    WEATHER_SETTINGS.setWindUnit(static_cast<WeatherWindUnit>((current + 1) % 4));
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  } else if (item.nameId == StrId::STR_WEATHER_PRECIP_UNIT) {
    auto current = WEATHER_SETTINGS.getPrecipUnit();
    WEATHER_SETTINGS.setPrecipUnit(current == WeatherPrecipUnit::MM ? WeatherPrecipUnit::INCH : WeatherPrecipUnit::MM);
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  }
}

void WeatherSettingsActivity::onSettingToggled(int index) {
  if (menuItems[index].nameId == StrId::STR_USE_WEATHER) {
    SETTINGS.saveToFile();
  }
}

void WeatherSettingsActivity::onBackPressed() {
  if (showingSearchResults) {
    showingSearchResults = false;
    requestUpdate();
    return;
  }
  finish();
}

void WeatherSettingsActivity::launchCitySearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_SEARCH_CITY), "",
                                                                 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto& kb = std::get<KeyboardResult>(result.data);
                           if (kb.text.empty()) return;

                           if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                             startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                                                    [this, query = kb.text](const ActivityResult& wifiResult) {
                                                      if (wifiResult.isCancelled) return;
                                                      searchResults = WeatherClient::searchCity(query);
                                                      showingSearchResults = !searchResults.empty();
                                                      selectedIndex = 0;
                                                      requestUpdate();
                                                    });
                           } else {
                             searchResults = WeatherClient::searchCity(kb.text);
                             showingSearchResults = !searchResults.empty();
                             selectedIndex = 0;
                             requestUpdate();
                           }
                         });
}

void WeatherSettingsActivity::launchLatitudeEntry() {
  std::string current = std::to_string(WEATHER_SETTINGS.getLatitude());
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LATITUDE),
                                                                 current, 12, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             char* end = nullptr;
                             const float lat = strtof(kb.text.c_str(), &end);
                             if (end != kb.text.c_str() && *end == '\0' && lat >= -90.0f && lat <= 90.0f) {
                               if (lat != WEATHER_SETTINGS.getLatitude()) {
                                 WEATHER_SETTINGS.setLocation(lat, WEATHER_SETTINGS.getLongitude(), "");
                               }
                               WEATHER_SETTINGS.saveToFile();
                               requestUpdate();
                             }
                           }
                         });
}

void WeatherSettingsActivity::launchLongitudeEntry() {
  std::string current = std::to_string(WEATHER_SETTINGS.getLongitude());
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LONGITUDE),
                                                                 current, 12, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             char* end = nullptr;
                             const float lon = strtof(kb.text.c_str(), &end);
                             if (end != kb.text.c_str() && *end == '\0' && lon >= -180.0f && lon <= 180.0f) {
                               if (lon != WEATHER_SETTINGS.getLongitude()) {
                                 WEATHER_SETTINGS.setLocation(WEATHER_SETTINGS.getLatitude(), lon, "");
                               }
                               WEATHER_SETTINGS.saveToFile();
                               requestUpdate();
                             }
                           }
                         });
}

void WeatherSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  if (showingSearchResults) {
    GUI.drawHeader(renderer,
                   Rect(contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight),
                   tr(STR_WEATHER_SEARCH_RESULTS));

    const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2);

    if (searchResults.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_NO_ENTRIES));
    } else {
      GUI.drawList(
          renderer, Rect(contentRect.x, contentTop, contentRect.width, contentHeight),
          static_cast<int>(searchResults.size()), static_cast<int>(selectedIndex),
          [this](int index) {
            const auto& r = searchResults[index];
            std::string label = r.name;
            if (!r.admin1.empty()) label += ", " + r.admin1;
            if (!r.country.empty()) label += ", " + r.country;
            return label;
          },
          nullptr, nullptr, nullptr, true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  GUI.drawHeader(renderer,
                 Rect(contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight),
                 tr(STR_WEATHER_SETTINGS));

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2);

  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
