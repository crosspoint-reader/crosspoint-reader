#include "WeatherIcons.h"

#include "WeatherIconsLarge.h"

// Weather icon glyph source attribution:
// https://github.com/erikflowers/weather-icons
// Large icons are provided by generated WI_LARGE_* arrays in WeatherIconsLarge.h.
// These large icons are 64x64, and the code uses WEATHER_ICON_SIZE=64.

// ============================================================================
// 24x24 Small Weather Icons (1-bit, MSB-first)
// Each row = 24 pixels = 3 bytes. Total = 24 * 3 = 72 bytes per icon.
// ============================================================================

// ============================================================================
// Lookup functions
// ============================================================================

const uint8_t* getWeatherIconLarge(WeatherIconType type) {
  switch (type) {
    case WeatherIconType::CLEAR_DAY:
      return WI_LARGE_CLEAR_DAY;
    case WeatherIconType::CLEAR_NIGHT:
      return WI_LARGE_CLEAR_NIGHT;
    case WeatherIconType::PARTLY_CLOUDY_DAY:
      return WI_LARGE_PARTLY_CLOUDY_DAY;
    case WeatherIconType::PARTLY_CLOUDY_NIGHT:
      return WI_LARGE_PARTLY_CLOUDY_NIGHT;
    case WeatherIconType::OVERCAST:
      return WI_LARGE_OVERCAST;
    case WeatherIconType::FOG:
      return WI_LARGE_FOG;
    case WeatherIconType::DRIZZLE:
      return WI_LARGE_DRIZZLE;
    case WeatherIconType::RAIN:
    case WeatherIconType::SHOWERS:
      return WI_LARGE_RAIN;
    case WeatherIconType::SNOW:
      return WI_LARGE_SNOW;
    case WeatherIconType::THUNDERSTORM:
      return WI_LARGE_THUNDERSTORM;
    default:
      return WI_LARGE_OVERCAST;
  }
}

const char* getWindDirectionText(int degrees) {
  // Normalize to 0-360
  degrees = ((degrees % 360) + 360) % 360;
  if (degrees >= 338 || degrees < 23) return "N";
  if (degrees < 68) return "NE";
  if (degrees < 113) return "E";
  if (degrees < 158) return "SE";
  if (degrees < 203) return "S";
  if (degrees < 248) return "SW";
  if (degrees < 293) return "W";
  return "NW";
}
