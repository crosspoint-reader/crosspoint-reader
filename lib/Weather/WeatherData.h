#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct CurrentWeather {
  float temperature = 0;
  float apparentTemperature = 0;
  int humidity = 0;
  int weatherCode = 0;
  float windSpeed = 0;
  int windDirection = 0;
  float precipitation = 0;
  float uvIndex = 0;
  bool isDay = true;
};

struct DailyForecast {
  time_t date = 0;
  float tempMax = 0;
  float tempMin = 0;
  int weatherCode = 0;
  float precipSum = 0;
  float uvIndexMax = 0;
  time_t sunrise = 0;
  time_t sunset = 0;
  float moonPhase = -1.0f;  // -1 means unknown, 0.0 new, 0.5 full
};

struct HourlyForecast {
  time_t time = 0;
  float temperature = 0;
  float precipitation = 0;
  int precipitationProbability = 0;
  int weatherCode = 0;
  bool isDay = true;
};

struct WeatherData {
  CurrentWeather current;
  std::vector<DailyForecast> daily;
  std::vector<HourlyForecast> hourly;
  std::string requestSignature;
  std::string timezone;
  int utcOffsetSeconds = 0;
  time_t fetchedAt = 0;
  bool valid = false;
  std::string errorMessage;
};

struct GeocodingResult {
  std::string name;
  std::string country;
  std::string admin1;  // State/region
  float latitude = 0;
  float longitude = 0;
};
