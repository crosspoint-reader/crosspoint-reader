#pragma once
#include <cstdint>

// Set to 1 (for example via build flag -DWEATHER_ENABLE_SMALL_ICONS=1)
// to include 24x24 weather icon assets and API.
#ifndef WEATHER_ENABLE_SMALL_ICONS
#define WEATHER_ENABLE_SMALL_ICONS 0
#endif

// Weather icon size constants
constexpr int WEATHER_ICON_LARGE = 64;  // Large icon for current weather
constexpr int WEATHER_ICON_SMALL = 24;  // Small icon for daily forecast

// WMO weather code to icon category mapping
enum class WeatherIconType {
  CLEAR_DAY,
  CLEAR_NIGHT,
  PARTLY_CLOUDY_DAY,
  PARTLY_CLOUDY_NIGHT,
  OVERCAST,
  FOG,
  DRIZZLE,
  RAIN,
  SNOW,
  SHOWERS,
  THUNDERSTORM,
  UNKNOWN
};

// Map WMO weather code + day/night to icon type
inline WeatherIconType getWeatherIconType(int wmoCode, bool isDay) {
  switch (wmoCode) {
    case 0:
      return isDay ? WeatherIconType::CLEAR_DAY : WeatherIconType::CLEAR_NIGHT;
    case 1:
    case 2:
      return isDay ? WeatherIconType::PARTLY_CLOUDY_DAY : WeatherIconType::PARTLY_CLOUDY_NIGHT;
    case 3:
      return WeatherIconType::OVERCAST;
    case 45:
    case 48:
      return WeatherIconType::FOG;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      return WeatherIconType::DRIZZLE;
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
      return WeatherIconType::RAIN;
    case 71:
    case 73:
    case 75:
    case 77:
      return WeatherIconType::SNOW;
    case 80:
    case 81:
    case 82:
    case 85:
    case 86:
      return WeatherIconType::SHOWERS;
    case 95:
    case 96:
    case 99:
      return WeatherIconType::THUNDERSTORM;
    default:
      return WeatherIconType::UNKNOWN;
  }
}

// Get human-readable description for WMO weather code
const char* getWeatherDescription(int wmoCode);

// Get the appropriate large icon bitmap (64x64, 1-bit, MSB first)
const uint8_t* getWeatherIconLarge(WeatherIconType type);

// Get the appropriate small icon bitmap (24x24, 1-bit, MSB first)
#if WEATHER_ENABLE_SMALL_ICONS
const uint8_t* getWeatherIconSmall(WeatherIconType type);
#endif

// Wind direction arrow text (N, NE, E, SE, S, SW, W, NW)
const char* getWindDirectionText(int degrees);
