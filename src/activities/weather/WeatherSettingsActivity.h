#pragma once

#include <WeatherData.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for weather configuration.
 * Supports city search via geocoding, manual lat/lon entry, and unit selection.
 */
class WeatherSettingsActivity final : public Activity {
 public:
  explicit WeatherSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WeatherSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  // City search results (populated when user searches)
  std::vector<GeocodingResult> searchResults;
  bool showingSearchResults = false;
  bool searchInProgress = false;
  std::string searchQuery;

  static constexpr int MENU_ITEMS = 6;  // Location, Lat, Lon, TempUnit, WindUnit, PrecipUnit

  void handleSelection();
  void launchCitySearch();
  void launchLatitudeEntry();
  void launchLongitudeEntry();
  void toggleTempUnit();
  void toggleWindUnit();
  void togglePrecipUnit();

  bool preventAutoSleep() override { return true; }
};
