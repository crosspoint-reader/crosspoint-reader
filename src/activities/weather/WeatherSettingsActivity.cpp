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

namespace {
const StrId menuNames[] = {
    StrId::STR_WEATHER_LOCATION,     // 0: Search city
    StrId::STR_WEATHER_LATITUDE,     // 1: Manual latitude
    StrId::STR_WEATHER_LONGITUDE,    // 2: Manual longitude
    StrId::STR_WEATHER_TEMP_UNIT,    // 3: Temperature unit
    StrId::STR_WEATHER_WIND_UNIT,    // 4: Wind speed unit
    StrId::STR_WEATHER_PRECIP_UNIT,  // 5: Precipitation unit
};
}  // namespace

void WeatherSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  showingSearchResults = false;
  requestUpdate();
}

void WeatherSettingsActivity::onExit() { Activity::onExit(); }

void WeatherSettingsActivity::loop() {
  if (showingSearchResults) {
    // Handle search results navigation
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      showingSearchResults = false;
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!searchResults.empty() && selectedIndex < searchResults.size()) {
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

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void WeatherSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case 0:
      launchCitySearch();
      break;
    case 1:
      launchLatitudeEntry();
      break;
    case 2:
      launchLongitudeEntry();
      break;
    case 3:
      toggleTempUnit();
      break;
    case 4:
      toggleWindUnit();
      break;
    case 5:
      togglePrecipUnit();
      break;
  }
}

void WeatherSettingsActivity::launchCitySearch() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_SEARCH_CITY), "", 64, false),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;

        const auto& kb = std::get<KeyboardResult>(result.data);
        if (kb.text.empty()) return;

        // Need WiFi for geocoding
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
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LATITUDE), current, 12, false),
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
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LONGITUDE), current, 12, false),
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

void WeatherSettingsActivity::toggleTempUnit() {
  auto current = WEATHER_SETTINGS.getTempUnit();
  WEATHER_SETTINGS.setTempUnit(current == WeatherTempUnit::CELSIUS ? WeatherTempUnit::FAHRENHEIT
                                                                   : WeatherTempUnit::CELSIUS);
  WEATHER_SETTINGS.saveToFile();
  requestUpdate();
}

void WeatherSettingsActivity::toggleWindUnit() {
  auto current = static_cast<uint8_t>(WEATHER_SETTINGS.getWindUnit());
  WEATHER_SETTINGS.setWindUnit(static_cast<WeatherWindUnit>((current + 1) % 4));
  WEATHER_SETTINGS.saveToFile();
  requestUpdate();
}

void WeatherSettingsActivity::togglePrecipUnit() {
  auto current = WEATHER_SETTINGS.getPrecipUnit();
  WEATHER_SETTINGS.setPrecipUnit(current == WeatherPrecipUnit::MM ? WeatherPrecipUnit::INCH : WeatherPrecipUnit::MM);
  WEATHER_SETTINGS.saveToFile();
  requestUpdate();
}

void WeatherSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (showingSearchResults) {
    // Display search results
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_WEATHER_SEARCH_RESULTS));

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

    if (searchResults.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(searchResults.size()),
          static_cast<int>(selectedIndex),
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

  // Main settings menu
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [this](int index) -> std::string {
        switch (index) {
          case 0: {
            auto name = WEATHER_SETTINGS.getLocationName();
            return name.empty() ? std::string(tr(STR_NOT_SET)) : name;
          }
          case 1: {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLatitude());
            return std::string(buf);
          }
          case 2: {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLongitude());
            return std::string(buf);
          }
          case 3:
            return WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
          case 4:
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
          case 5:
            return WEATHER_SETTINGS.getPrecipUnit() == WeatherPrecipUnit::MM ? "mm" : "in";
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
