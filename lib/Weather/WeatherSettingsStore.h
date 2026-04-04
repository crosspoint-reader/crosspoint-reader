#pragma once
#include <cstdint>
#include <string>

enum class WeatherTempUnit : uint8_t { CELSIUS = 0, FAHRENHEIT = 1 };
enum class WeatherWindUnit : uint8_t { KMH = 0, MS = 1, MPH = 2, KNOTS = 3 };
enum class WeatherPrecipUnit : uint8_t { MM = 0, INCH = 1 };

class WeatherSettingsStore {
 private:
  static WeatherSettingsStore instance;

  float latitude = 0;
  float longitude = 0;
  bool locationConfigured = false;
  std::string locationName;
  WeatherTempUnit tempUnit = WeatherTempUnit::CELSIUS;
  WeatherWindUnit windUnit = WeatherWindUnit::KMH;
  WeatherPrecipUnit precipUnit = WeatherPrecipUnit::MM;
  uint8_t forecastDays = 3;

  WeatherSettingsStore() = default;

 public:
  WeatherSettingsStore(const WeatherSettingsStore&) = delete;
  WeatherSettingsStore& operator=(const WeatherSettingsStore&) = delete;

  static WeatherSettingsStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  // Location
  void setLocation(float lat, float lon, const std::string& name);
  void clearLocation();
  float getLatitude() const { return latitude; }
  float getLongitude() const { return longitude; }
  const std::string& getLocationName() const { return locationName; }
  bool hasLocation() const { return locationConfigured; }

  // Units
  void setTempUnit(WeatherTempUnit unit) { tempUnit = unit; }
  WeatherTempUnit getTempUnit() const { return tempUnit; }
  void setWindUnit(WeatherWindUnit unit) { windUnit = unit; }
  WeatherWindUnit getWindUnit() const { return windUnit; }
  void setPrecipUnit(WeatherPrecipUnit unit) { precipUnit = unit; }
  WeatherPrecipUnit getPrecipUnit() const { return precipUnit; }

  // Forecast
  void setForecastDays(uint8_t days);
  uint8_t getForecastDays() const { return forecastDays; }

  // API parameter strings
  const char* getTempUnitParam() const;
  const char* getWindUnitParam() const;
  const char* getPrecipUnitParam() const;
};

#define WEATHER_SETTINGS WeatherSettingsStore::getInstance()
