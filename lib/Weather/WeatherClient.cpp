#include "WeatherClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <ctime>

#include "../../src/network/HttpDownloader.h"

namespace {
constexpr char WEATHER_CACHE_FILE[] = "/.crosspoint/weather_cache.json";

std::string buildForecastUrl(const WeatherSettingsStore& settings) {
  // Open-Meteo supports plain HTTP. Using HTTP here avoids TLS handshake
  // instability observed on some ESP32-C3 builds.
  std::string url = "http://api.open-meteo.com/v1/forecast?";
  url += "latitude=" + std::to_string(settings.getLatitude());
  url += "&longitude=" + std::to_string(settings.getLongitude());
  url +=
      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
      "weather_code,wind_speed_10m,wind_direction_10m,is_day,precipitation,uv_index";
  url += "&hourly=temperature_2m,precipitation,precipitation_probability,weather_code,is_day";
  url +=
      "&daily=temperature_2m_max,temperature_2m_min,weather_code,"
      "precipitation_sum,sunrise,sunset,uv_index_max";
  url += "&timezone=auto&timeformat=unixtime";
  url += std::string("&temperature_unit=") + settings.getTempUnitParam();
  url += std::string("&wind_speed_unit=") + settings.getWindUnitParam();
  url += std::string("&precipitation_unit=") + settings.getPrecipUnitParam();
  url += "&forecast_days=" + std::to_string(settings.getForecastDays());
  // Request 48 hours of hourly data
  url += "&forecast_hours=48";
  return url;
}
}  // namespace

WeatherData WeatherClient::getWeather(const WeatherSettingsStore& settings, bool forceRefresh) {
  if (!settings.hasLocation()) {
    WeatherData data;
    data.errorMessage = "No location configured";
    return data;
  }

  if (!forceRefresh) {
    WeatherData cached;
    if (loadCache(cached) && cached.valid) {
      time_t now;
      time(&now);
      if (now - cached.fetchedAt < CACHE_TTL_SECONDS) {
        LOG_DBG("WEA", "Using cached weather data (age: %ld s)", (long)(now - cached.fetchedAt));
        return cached;
      }
      LOG_DBG("WEA", "Cache expired (age: %ld s)", (long)(now - cached.fetchedAt));
      // Cache-first behavior: return stale cache and let the caller decide
      // whether/when to perform a network refresh.
      return cached;
    }

    LOG_DBG("WEA", "No cache available; caller should establish network and force refresh");
    WeatherData data;
    data.errorMessage = "No cache";
    return data;
  }

  return fetchFromApi(settings);
}

WeatherData WeatherClient::fetchFromApi(const WeatherSettingsStore& settings) {
  WeatherData data;
  std::string url = buildForecastUrl(settings);
  LOG_DBG("WEA", "fetchFromApi[1] start");
  LOG_DBG("WEA", "fetchFromApi[2] url length=%zu", url.size());
  LOG_DBG("WEA", "Fetching weather from API");

  std::string response;
  LOG_DBG("WEA", "fetchFromApi[3] HttpDownloader::fetchUrl before");
  if (!HttpDownloader::fetchUrl(url, response)) {
    data.errorMessage = "Network error";
    LOG_ERR("WEA", "Failed to fetch weather data");
    return data;
  }
  LOG_DBG("WEA", "fetchFromApi[4] HttpDownloader::fetchUrl after; bytes=%zu", response.size());

  LOG_DBG("WEA", "fetchFromApi[5] parseWeatherJson before");
  if (!parseWeatherJson(response, data)) {
    LOG_ERR("WEA", "Failed to parse weather JSON");
    return data;
  }
  LOG_DBG("WEA", "fetchFromApi[6] parseWeatherJson after");

  data.valid = true;
  time(&data.fetchedAt);
  saveCache(data);
  LOG_DBG("WEA", "fetchFromApi[7] Weather data fetched and cached");
  return data;
}

bool WeatherClient::parseWeatherJson(const std::string& json, WeatherData& data) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    data.errorMessage = "JSON parse error";
    LOG_ERR("WEA", "JSON parse error: %s", error.c_str());
    return false;
  }

  // Check for API error
  if (doc["error"] | false) {
    data.errorMessage = doc["reason"] | "API error";
    LOG_ERR("WEA", "API error: %s", data.errorMessage.c_str());
    return false;
  }

  data.timezone = doc["timezone"] | std::string("");
  data.utcOffsetSeconds = doc["utc_offset_seconds"] | 0;

  // Parse current weather
  JsonObject current = doc["current"];
  if (current) {
    data.current.temperature = current["temperature_2m"] | 0.0f;
    data.current.apparentTemperature = current["apparent_temperature"] | 0.0f;
    data.current.humidity = current["relative_humidity_2m"] | 0;
    data.current.weatherCode = current["weather_code"] | 0;
    data.current.windSpeed = current["wind_speed_10m"] | 0.0f;
    data.current.windDirection = current["wind_direction_10m"] | 0;
    data.current.precipitation = current["precipitation"] | 0.0f;
    data.current.uvIndex = current["uv_index"] | 0.0f;
    data.current.isDay = (current["is_day"] | 1) != 0;
  }

  // Parse daily forecast
  JsonObject daily = doc["daily"];
  if (daily) {
    JsonArray times = daily["time"];
    JsonArray tempMax = daily["temperature_2m_max"];
    JsonArray tempMin = daily["temperature_2m_min"];
    JsonArray codes = daily["weather_code"];
    JsonArray precip = daily["precipitation_sum"];
    JsonArray sunrise = daily["sunrise"];
    JsonArray sunset = daily["sunset"];
    JsonArray uvMax = daily["uv_index_max"];

    size_t count = times.size();
    data.daily.reserve(count);
    for (size_t i = 0; i < count; i++) {
      DailyForecast day;
      day.date = times[i] | (time_t)0;
      day.tempMax = tempMax[i] | 0.0f;
      day.tempMin = tempMin[i] | 0.0f;
      day.weatherCode = codes[i] | 0;
      day.precipSum = precip[i] | 0.0f;
      day.sunrise = sunrise[i] | (time_t)0;
      day.sunset = sunset[i] | (time_t)0;
      day.uvIndexMax = uvMax[i] | 0.0f;
      data.daily.push_back(day);
    }
  }

  // Parse hourly forecast
  JsonObject hourly = doc["hourly"];
  if (hourly) {
    JsonArray times = hourly["time"];
    JsonArray temp = hourly["temperature_2m"];
    JsonArray precip = hourly["precipitation"];
    JsonArray precipProb = hourly["precipitation_probability"];
    JsonArray codes = hourly["weather_code"];
    JsonArray isDay = hourly["is_day"];

    size_t count = times.size();
    data.hourly.reserve(count);
    for (size_t i = 0; i < count; i++) {
      HourlyForecast hour;
      hour.time = times[i] | (time_t)0;
      hour.temperature = temp[i] | 0.0f;
      hour.precipitation = precip[i] | 0.0f;
      hour.precipitationProbability = precipProb[i] | 0;
      hour.weatherCode = codes[i] | 0;
      hour.isDay = (isDay[i] | 1) != 0;
      data.hourly.push_back(hour);
    }
  }

  return true;
}

bool WeatherClient::saveCache(const WeatherData& data) {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["fetchedAt"] = data.fetchedAt;
  doc["timezone"] = data.timezone;
  doc["utcOffsetSeconds"] = data.utcOffsetSeconds;

  // Current
  JsonObject cur = doc["current"].to<JsonObject>();
  cur["temperature"] = data.current.temperature;
  cur["apparentTemperature"] = data.current.apparentTemperature;
  cur["humidity"] = data.current.humidity;
  cur["weatherCode"] = data.current.weatherCode;
  cur["windSpeed"] = data.current.windSpeed;
  cur["windDirection"] = data.current.windDirection;
  cur["precipitation"] = data.current.precipitation;
  cur["uvIndex"] = data.current.uvIndex;
  cur["isDay"] = data.current.isDay;

  // Daily
  JsonArray dailyArr = doc["daily"].to<JsonArray>();
  for (const auto& day : data.daily) {
    JsonObject d = dailyArr.add<JsonObject>();
    d["date"] = day.date;
    d["tempMax"] = day.tempMax;
    d["tempMin"] = day.tempMin;
    d["weatherCode"] = day.weatherCode;
    d["precipSum"] = day.precipSum;
    d["uvIndexMax"] = day.uvIndexMax;
    d["sunrise"] = day.sunrise;
    d["sunset"] = day.sunset;
  }

  // Hourly
  JsonArray hourlyArr = doc["hourly"].to<JsonArray>();
  for (const auto& hour : data.hourly) {
    JsonObject h = hourlyArr.add<JsonObject>();
    h["time"] = hour.time;
    h["temperature"] = hour.temperature;
    h["precipitation"] = hour.precipitation;
    h["precipProb"] = hour.precipitationProbability;
    h["weatherCode"] = hour.weatherCode;
    h["isDay"] = hour.isDay;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(WEATHER_CACHE_FILE, json);
}

bool WeatherClient::loadCache(WeatherData& data) {
  if (!Storage.exists(WEATHER_CACHE_FILE)) {
    return false;
  }

  String json = Storage.readFile(WEATHER_CACHE_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WEA", "Cache parse error: %s", error.c_str());
    return false;
  }

  data.fetchedAt = doc["fetchedAt"] | (time_t)0;
  data.timezone = doc["timezone"] | std::string("");
  data.utcOffsetSeconds = doc["utcOffsetSeconds"] | 0;

  // Current
  JsonObject cur = doc["current"];
  if (cur) {
    data.current.temperature = cur["temperature"] | 0.0f;
    data.current.apparentTemperature = cur["apparentTemperature"] | 0.0f;
    data.current.humidity = cur["humidity"] | 0;
    data.current.weatherCode = cur["weatherCode"] | 0;
    data.current.windSpeed = cur["windSpeed"] | 0.0f;
    data.current.windDirection = cur["windDirection"] | 0;
    data.current.precipitation = cur["precipitation"] | 0.0f;
    data.current.uvIndex = cur["uvIndex"] | 0.0f;
    data.current.isDay = cur["isDay"] | true;
  }

  // Daily
  JsonArray dailyArr = doc["daily"].as<JsonArray>();
  for (JsonObject d : dailyArr) {
    DailyForecast day;
    day.date = d["date"] | (time_t)0;
    day.tempMax = d["tempMax"] | 0.0f;
    day.tempMin = d["tempMin"] | 0.0f;
    day.weatherCode = d["weatherCode"] | 0;
    day.precipSum = d["precipSum"] | 0.0f;
    day.uvIndexMax = d["uvIndexMax"] | 0.0f;
    day.sunrise = d["sunrise"] | (time_t)0;
    day.sunset = d["sunset"] | (time_t)0;
    data.daily.push_back(day);
  }

  // Hourly
  JsonArray hourlyArr = doc["hourly"].as<JsonArray>();
  for (JsonObject h : hourlyArr) {
    HourlyForecast hour;
    hour.time = h["time"] | (time_t)0;
    hour.temperature = h["temperature"] | 0.0f;
    hour.precipitation = h["precipitation"] | 0.0f;
    hour.precipitationProbability = h["precipProb"] | 0;
    hour.weatherCode = h["weatherCode"] | 0;
    hour.isDay = h["isDay"] | true;
    data.hourly.push_back(hour);
  }

  data.valid = true;
  LOG_DBG("WEA", "Cache loaded, fetchedAt=%ld", (long)data.fetchedAt);
  return true;
}

std::vector<GeocodingResult> WeatherClient::searchCity(const std::string& query) {
  std::vector<GeocodingResult> results;

  std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=";
  // Simple URL encoding for the query
  for (char c : query) {
    if (c == ' ') {
      url += "%20";
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.') {
      url += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      url += buf;
    }
  }
  url += "&count=5&language=en";

  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    LOG_ERR("WEA", "Geocoding request failed");
    return results;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, response);
  if (error) {
    LOG_ERR("WEA", "Geocoding parse error: %s", error.c_str());
    return results;
  }

  JsonArray arr = doc["results"].as<JsonArray>();
  for (JsonObject obj : arr) {
    GeocodingResult r;
    r.name = obj["name"] | std::string("");
    r.country = obj["country"] | std::string("");
    r.admin1 = obj["admin1"] | std::string("");
    r.latitude = obj["latitude"] | 0.0f;
    r.longitude = obj["longitude"] | 0.0f;
    results.push_back(r);
  }

  LOG_DBG("WEA", "Geocoding found %zu results for '%s'", results.size(), query.c_str());
  return results;
}
