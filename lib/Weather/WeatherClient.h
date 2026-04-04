#pragma once
#include <string>

#include "WeatherData.h"
#include "WeatherSettingsStore.h"

class WeatherClient {
 public:
  /// Fetch weather data, using cache if valid (< 30 min old).
  /// If forceRefresh is true, always fetches from the API.
  static WeatherData getWeather(const WeatherSettingsStore& settings, bool forceRefresh = false);

  /// Search for cities by name via Open-Meteo geocoding API.
  /// Returns up to 5 results.
  static std::vector<GeocodingResult> searchCity(const std::string& query);

 private:
  static WeatherData fetchFromApi(const WeatherSettingsStore& settings);
  static bool parseWeatherJson(const std::string& json, WeatherData& data);
  static std::string buildRequestSignature(const WeatherSettingsStore& settings);
  static bool saveCache(const WeatherData& data);
  static bool loadCache(WeatherData& data);

  static constexpr int CACHE_TTL_SECONDS = 30 * 60;  // 30 minutes
};
