#pragma once

#include <WeatherData.h>

#include <string>
#include <vector>

#include "../MenuListActivity.h"

/**
 * Settings submenu for weather configuration.
 * Supports city search via geocoding, manual lat/lon entry, and unit selection.
 */
class WeatherSettingsActivity final : public MenuListActivity {
 public:
  explicit WeatherSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : MenuListActivity("WeatherSettings", renderer, mappedInput) {
    buildMenuItems();
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<GeocodingResult> searchResults;
  bool showingSearchResults = false;
  std::string searchQuery;

  void buildMenuItems();
  void onActionSelected(int index) override;
  std::string getItemValueString(int index) const override;
  void onBackPressed() override;
  void onSettingToggled(int index) override;

  void launchCitySearch();
  void launchLatitudeEntry();
  void launchLongitudeEntry();
};
