#include "WeatherSettingsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

WeatherSettingsStore WeatherSettingsStore::instance;

namespace {
constexpr char WEATHER_SETTINGS_FILE[] = "/.crosspoint/weather_settings.json";
}

bool WeatherSettingsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["latitude"] = latitude;
  doc["longitude"] = longitude;
  doc["locationConfigured"] = locationConfigured;
  doc["locationName"] = locationName;
  doc["tempUnit"] = static_cast<uint8_t>(tempUnit);
  doc["windUnit"] = static_cast<uint8_t>(windUnit);
  doc["precipUnit"] = static_cast<uint8_t>(precipUnit);
  doc["forecastDays"] = forecastDays;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(WEATHER_SETTINGS_FILE, json);
}

bool WeatherSettingsStore::loadFromFile() {
  if (!Storage.exists(WEATHER_SETTINGS_FILE)) {
    LOG_DBG("WEA", "No weather settings file found");
    return false;
  }

  String json = Storage.readFile(WEATHER_SETTINGS_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WEA", "JSON parse error: %s", error.c_str());
    return false;
  }

  latitude = doc["latitude"] | 0.0f;
  longitude = doc["longitude"] | 0.0f;
  locationConfigured = doc["locationConfigured"] | (latitude != 0.0f || longitude != 0.0f);
  locationName = doc["locationName"] | std::string("");
  tempUnit = static_cast<WeatherTempUnit>(doc["tempUnit"] | (uint8_t)0);
  windUnit = static_cast<WeatherWindUnit>(doc["windUnit"] | (uint8_t)0);
  precipUnit = static_cast<WeatherPrecipUnit>(doc["precipUnit"] | (uint8_t)0);
  forecastDays = doc["forecastDays"] | (uint8_t)3;

  if (forecastDays < 1) forecastDays = 1;
  if (forecastDays > 5) forecastDays = 5;

  LOG_DBG("WEA", "Loaded weather settings: %s (%.2f, %.2f)", locationName.c_str(), latitude, longitude);
  return true;
}

void WeatherSettingsStore::setLocation(float lat, float lon, const std::string& name) {
  latitude = lat;
  longitude = lon;
  locationConfigured = true;
  locationName = name;
  LOG_DBG("WEA", "Set location: %s (%.4f, %.4f)", name.c_str(), lat, lon);
}

void WeatherSettingsStore::clearLocation() {
  latitude = 0;
  longitude = 0;
  locationConfigured = false;
  locationName.clear();
}

void WeatherSettingsStore::setForecastDays(uint8_t days) {
  if (days < 1) days = 1;
  if (days > 5) days = 5;
  forecastDays = days;
}

const char* WeatherSettingsStore::getTempUnitParam() const {
  return tempUnit == WeatherTempUnit::FAHRENHEIT ? "fahrenheit" : "celsius";
}

const char* WeatherSettingsStore::getWindUnitParam() const {
  switch (windUnit) {
    case WeatherWindUnit::MS:
      return "ms";
    case WeatherWindUnit::MPH:
      return "mph";
    case WeatherWindUnit::KNOTS:
      return "kn";
    case WeatherWindUnit::KMH:
    default:
      return "kmh";
  }
}

const char* WeatherSettingsStore::getPrecipUnitParam() const {
  return precipUnit == WeatherPrecipUnit::INCH ? "inch" : "mm";
}
