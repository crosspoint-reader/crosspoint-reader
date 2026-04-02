#include "WeatherActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WeatherClient.h>
#include <WeatherIcons.h>
#include <WeatherSettingsStore.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#include "MappedInputManager.h"
#include "WeatherSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
inline bool getBitmapBit(const uint8_t* bitmap, const int size, const int x, const int y) {
  const int rowBytes = size / 8;
  const int idx = y * rowBytes + (x / 8);
  const uint8_t mask = static_cast<uint8_t>(0x80 >> (x % 8));
  return (bitmap[idx] & mask) != 0;
}

void drawWeatherIconWithOrientation(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y,
                                    const int size) {
  for (int srcY = 0; srcY < size; srcY++) {
    for (int srcX = 0; srcX < size; srcX++) {
      // 0 bits are black (drawn) in these icon bitmaps.
      if (getBitmapBit(icon, size, srcX, srcY)) {
        continue;
      }

      renderer.drawPixel(x + srcX, y + srcY);
    }
  }
}

StrId getWeatherDescriptionStrId(const int wmoCode) {
  switch (wmoCode) {
    case 0:
      return StrId::STR_WEATHER_DESC_CLEAR_SKY;
    case 1:
      return StrId::STR_WEATHER_DESC_MAINLY_CLEAR;
    case 2:
      return StrId::STR_WEATHER_DESC_PARTLY_CLOUDY;
    case 3:
      return StrId::STR_WEATHER_DESC_OVERCAST;
    case 45:
      return StrId::STR_WEATHER_DESC_FOG;
    case 48:
      return StrId::STR_WEATHER_DESC_RIME_FOG;
    case 51:
      return StrId::STR_WEATHER_DESC_LIGHT_DRIZZLE;
    case 53:
      return StrId::STR_WEATHER_DESC_DRIZZLE;
    case 55:
      return StrId::STR_WEATHER_DESC_DENSE_DRIZZLE;
    case 56:
      return StrId::STR_WEATHER_DESC_FREEZING_DRIZZLE;
    case 57:
      return StrId::STR_WEATHER_DESC_DENSE_FREEZING_DRIZZLE;
    case 61:
      return StrId::STR_WEATHER_DESC_SLIGHT_RAIN;
    case 63:
      return StrId::STR_WEATHER_DESC_MODERATE_RAIN;
    case 65:
      return StrId::STR_WEATHER_DESC_HEAVY_RAIN;
    case 66:
      return StrId::STR_WEATHER_DESC_FREEZING_RAIN;
    case 67:
      return StrId::STR_WEATHER_DESC_HEAVY_FREEZING_RAIN;
    case 71:
      return StrId::STR_WEATHER_DESC_SLIGHT_SNOW;
    case 73:
      return StrId::STR_WEATHER_DESC_MODERATE_SNOW;
    case 75:
      return StrId::STR_WEATHER_DESC_HEAVY_SNOW;
    case 77:
      return StrId::STR_WEATHER_DESC_SNOW_GRAINS;
    case 80:
      return StrId::STR_WEATHER_DESC_SLIGHT_SHOWERS;
    case 81:
      return StrId::STR_WEATHER_DESC_MODERATE_SHOWERS;
    case 82:
      return StrId::STR_WEATHER_DESC_VIOLENT_SHOWERS;
    case 85:
      return StrId::STR_WEATHER_DESC_SNOW_SHOWERS;
    case 86:
      return StrId::STR_WEATHER_DESC_HEAVY_SNOW_SHOWERS;
    case 95:
      return StrId::STR_WEATHER_DESC_THUNDERSTORM;
    case 96:
      return StrId::STR_WEATHER_DESC_THUNDERSTORM_HAIL;
    case 99:
      return StrId::STR_WEATHER_DESC_THUNDERSTORM_HEAVY_HAIL;
    default:
      return StrId::STR_WEATHER_DESC_UNKNOWN;
  }
}
}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();

  // Force landscape orientation for weather display
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);

  state = State::LOADING_CACHE;
  errorMessage.clear();
  forceRefresh = false;
  showRefreshPopup = false;
  requestUpdate();

  loadAndDisplay();
}

void WeatherActivity::onExit() {
  Activity::onExit();

  // Restore orientation from settings
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(SETTINGS.orientation));

  WiFi.mode(WIFI_OFF);
}

void WeatherActivity::loadAndDisplay() {
  constexpr long CACHE_REFRESH_AGE_SECONDS = 30L * 60L;

  if (!WEATHER_SETTINGS.hasLocation()) {
    state = State::ERROR;
    errorMessage = tr(STR_WEATHER_NO_LOCATION);
    requestUpdate();
    return;
  }

  // Try cache first
  WeatherData cached = WeatherClient::getWeather(WEATHER_SETTINGS, forceRefresh);
  if (cached.valid) {
    const time_t now = time(nullptr);
    const long age = (cached.fetchedAt > 0) ? static_cast<long>(now - cached.fetchedAt) : -1;
    const bool cacheIsStale = (age >= 0 && age > CACHE_REFRESH_AGE_SECONDS) || age < 0;
    const bool shouldBackgroundRefresh = forceRefresh || cacheIsStale;

    if (cacheIsStale) {
      LOG_DBG("WEA", "Displayed stale cache (age=%ld s); scheduling refresh", age);
    } else {
      LOG_DBG("WEA", "Displayed fresh cache (age=%ld s); skip auto-refresh", age);
    }

    weatherData = std::move(cached);
    state = State::WEATHER_DISPLAY;
    requestUpdate();

    // Refresh only when forced or when cache is stale.
    if (shouldBackgroundRefresh) {
      checkAndConnectWifi();
    }
    return;
  }

  // Need fresh data - check WiFi
  LOG_DBG("WEA", "No cache to display; proceeding to WiFi/network fetch path");
  checkAndConnectWifi();
}

void WeatherActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::FETCHING;
    requestUpdate(true);
    fetchWeather();
    return;
  }

  launchWifiSelection();
}

void WeatherActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeatherActivity::onWifiSelectionComplete(bool connected) {
  // Re-apply landscape after returning from WiFi selection (which uses portrait)
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  LOG_DBG("WEA", "onWifiSelectionComplete connected=%d wifiStatus=%d", connected ? 1 : 0, (int)WiFi.status());

  if (connected) {
    state = State::CHECK_WIFI;
    wifiWaitStartedAtMs = millis();
    LOG_DBG("WEA", "state -> CHECK_WIFI (waitStart=%lu)", (unsigned long)wifiWaitStartedAtMs);
    requestUpdate(true);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

void WeatherActivity::fetchWeather() {
  LOG_DBG("WEA", "fetchWeather[1] enter status=%d ip=%s", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    LOG_ERR("WEA", "fetchWeather[2] WiFi not ready status=%d ip=%s", (int)WiFi.status(),
            WiFi.localIP().toString().c_str());
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  LOG_DBG("WEA", "fetchWeather[3] WeatherClient::getWeather before");
  weatherData = WeatherClient::getWeather(WEATHER_SETTINGS, true);
  LOG_DBG("WEA", "fetchWeather[4] WeatherClient::getWeather after valid=%d", weatherData.valid ? 1 : 0);
  if (weatherData.valid) {
    state = State::WEATHER_DISPLAY;
  } else {
    state = State::ERROR;
    errorMessage = weatherData.errorMessage.empty() ? tr(STR_WEATHER_FETCH_FAILED) : weatherData.errorMessage.c_str();
  }
  showRefreshPopup = false;
  requestUpdate();
}

void WeatherActivity::loop() {
  if (state == State::WIFI_SELECTION) {
    return;  // Handled by WifiSelectionActivity
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Open weather settings (useful when no location is configured)
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      startActivityForResult(std::make_unique<WeatherSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
                               forceRefresh = true;
                               loadAndDisplay();
                             });
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      // Retry fetch
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = State::FETCHING;
        requestUpdate(true);
        fetchWeather();
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::LOADING_CACHE || state == State::CHECK_WIFI || state == State::FETCHING) {
    if (state == State::CHECK_WIFI) {
      LOG_DBG("WEA", "loop CHECK_WIFI status=%d ip=%s elapsed=%lu", (int)WiFi.status(),
              WiFi.localIP().toString().c_str(), (unsigned long)(millis() - wifiWaitStartedAtMs));
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        // Give lwIP/sockets a short window to settle after activity handoff.
        if (wifiWaitStartedAtMs > 0 && millis() - wifiWaitStartedAtMs < 1200) {
          LOG_DBG("WEA", "loop CHECK_WIFI waiting for settle window");
          return;
        }
        LOG_DBG("WEA", "state CHECK_WIFI -> FETCHING");
        state = State::FETCHING;
        requestUpdate(true);
        fetchWeather();
        return;
      }

      if (wifiWaitStartedAtMs > 0 && millis() - wifiWaitStartedAtMs > 15000) {
        LOG_ERR("WEA", "CHECK_WIFI timeout after %lu ms", (unsigned long)(millis() - wifiWaitStartedAtMs));
        state = State::ERROR;
        errorMessage = tr(STR_WIFI_CONN_FAILED);
        requestUpdate();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  // WEATHER_DISPLAY state
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Open weather settings
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    startActivityForResult(std::make_unique<WeatherSettingsActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             // Re-apply landscape after returning from settings
                             renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
                             // Refresh after settings change
                             forceRefresh = true;
                             loadAndDisplay();
                           });
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    // Manual refresh with popup feedback.
    showRefreshPopup = true;
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      state = State::FETCHING;
      requestUpdate(true);
      fetchWeather();
    } else {
      launchWifiSelection();
    }
  }
}

void WeatherActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();    // 800 in landscape
  const auto pageHeight = renderer.getScreenHeight();  // 480 in landscape

  if (state == State::LOADING_CACHE || (state == State::FETCHING && (!showRefreshPopup || !weatherData.valid))) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_SETTINGS), tr(STR_WEATHER_REFRESH), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // DISPLAY state - Landscape layout (800x480 logical)
  //
  // +------------------+------------------------------------+
  // |  Current weather  |    Daily forecast cards            |
  // |  (icon, temp,     |    [Day1] [Day2] [Day3] ...       |
  // |   details)        |                                    |
  // | leftPanelWidth=220| right panel = contentWidth-220    |
  // +------------------+------------------------------------+
  // |                                                        |
  // | hourly temperature + precipitation graph               |
  // | height = graphHeight (currently 240)                  |
  // +--------------------------------------------------------+
  // |   [Back]  [Settings]  [Refresh]  Button hints          |
  // +--------------------------------------------------------+

  const auto& metrics = UITheme::getInstance().getMetrics();
  // In LandscapeClockwise, physical bottom buttons are at the logical LEFT edge
  const int contentX = metrics.buttonHintsHeight;
  const int contentWidth = pageWidth - contentX;
  constexpr int leftPanelWidth = 220;
  constexpr int graphHeight = 240;
  const int topSectionHeight = pageHeight - graphHeight;

  // Draw current conditions (left panel, offset by button hints)
  renderCurrentConditions(contentX, 0, leftPanelWidth, topSectionHeight);

  // Draw daily forecast (right of current conditions)
  renderDailyForecast(contentX + leftPanelWidth, 0, contentWidth - leftPanelWidth, topSectionHeight);

  // Separator line
  renderer.drawLine(contentX, topSectionHeight, pageWidth, topSectionHeight);

  // Draw hourly graph
  renderHourlyGraph(contentX, topSectionHeight + 1, contentWidth, graphHeight - 1);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_SETTINGS_SHORT), tr(STR_WEATHER_REFRESH), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (state == State::FETCHING && showRefreshPopup) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  }

  renderer.displayBuffer();
}

void WeatherActivity::renderCurrentConditions(int x, int y, int w, int h) {
  const auto& cur = weatherData.current;

  // Location name
  int textY = y + 15;
  auto locationName = WEATHER_SETTINGS.getLocationName();
  if (!locationName.empty()) {
    auto truncated = renderer.truncatedText(UI_10_FONT_ID, locationName.c_str(), w - 10);
    renderer.drawText(UI_10_FONT_ID, x + 5, textY, truncated.c_str(), true, EpdFontFamily::BOLD);
    textY += 20;
  }

  // Last updated time
  if (weatherData.fetchedAt > 0) {
    struct tm timeinfo;
    time_t localTime = weatherData.fetchedAt + weatherData.utcOffsetSeconds;
    gmtime_r(&localTime, &timeinfo);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    renderer.drawText(SMALL_FONT_ID, x + 5, textY, timeBuf);
    textY += 15;
  }

  // Weather icon (WEATHER_ICON_LARGE x WEATHER_ICON_LARGE)
  auto iconType = getWeatherIconType(cur.weatherCode, cur.isDay);
  const uint8_t* icon = getWeatherIconLarge(iconType);
  int iconX = x + (w - WEATHER_ICON_LARGE) / 2;
  drawWeatherIconWithOrientation(renderer, icon, iconX, textY + 2, WEATHER_ICON_LARGE);
  textY += WEATHER_ICON_LARGE + 5;

  // Temperature (large)
  char tempBuf[16];
  const char* unitSuffix = WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
  snprintf(tempBuf, sizeof(tempBuf), "%.1f %s", cur.temperature, unitSuffix);
  int tempWidth = renderer.getTextWidth(UI_12_FONT_ID, tempBuf, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - tempWidth) / 2, textY, tempBuf, true, EpdFontFamily::BOLD);
  textY += 22;

  // Feels like
  snprintf(tempBuf, sizeof(tempBuf), "%.1f %s", cur.apparentTemperature, unitSuffix);
  char feelsLikeBuf[48];
  snprintf(feelsLikeBuf, sizeof(feelsLikeBuf), "%s: %s", tr(STR_WEATHER_FEELS_LIKE), tempBuf);
  auto feelsText = renderer.truncatedText(SMALL_FONT_ID, feelsLikeBuf, w - 10);
  int feelsWidth = renderer.getTextWidth(SMALL_FONT_ID, feelsText.c_str());
  renderer.drawText(SMALL_FONT_ID, x + (w - feelsWidth) / 2, textY, feelsText.c_str());
  textY += 16;

  // Weather description
  const char* desc = I18N.get(getWeatherDescriptionStrId(cur.weatherCode));
  int descWidth = renderer.getTextWidth(SMALL_FONT_ID, desc);
  renderer.drawText(SMALL_FONT_ID, x + (w - descWidth) / 2, textY, desc);
  textY += 20;

  // Details grid
  const char* windDir = getWindDirectionText(cur.windDirection);
  char detailBuf[64];
  const auto drawCenteredDetail = [&](const char* text) {
    auto truncated = renderer.truncatedText(SMALL_FONT_ID, text, w - 10);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (w - textWidth) / 2, textY, truncated.c_str());
  };

  // Wind
  snprintf(detailBuf, sizeof(detailBuf), "%s: %.0f %s %s", tr(STR_WEATHER_WIND), cur.windSpeed,
           WEATHER_SETTINGS.getWindUnitParam(), windDir);
  drawCenteredDetail(detailBuf);
  textY += 14;

  // Humidity
  snprintf(detailBuf, sizeof(detailBuf), "%s: %d%%", tr(STR_WEATHER_HUMIDITY), cur.humidity);
  drawCenteredDetail(detailBuf);
  textY += 14;

  // Precipitation
  if (std::fabs(cur.precipitation) < 0.0001f) {
    snprintf(detailBuf, sizeof(detailBuf), "%s: --", tr(STR_WEATHER_PRECIP));
  } else {
    snprintf(detailBuf, sizeof(detailBuf), "%s: %.1f %s", tr(STR_WEATHER_PRECIP), cur.precipitation,
             WEATHER_SETTINGS.getPrecipUnitParam());
  }
  drawCenteredDetail(detailBuf);
  textY += 14;

  // UV Index
  snprintf(detailBuf, sizeof(detailBuf), "UV: %.1f", cur.uvIndex);
  drawCenteredDetail(detailBuf);

  // Vertical separator
  renderer.drawLine(x + w, y, x + w, y + h);
}

void WeatherActivity::renderDailyForecast(int x, int y, int w, int h) {
  if (weatherData.daily.empty()) return;

  // Localized weekday names (indexed by tm_wday: 0=Sun..6=Sat)
  const StrId dayNameIds[] = {StrId::STR_WEATHER_DAY_SUN, StrId::STR_WEATHER_DAY_MON, StrId::STR_WEATHER_DAY_TUE,
                              StrId::STR_WEATHER_DAY_WED, StrId::STR_WEATHER_DAY_THU, StrId::STR_WEATHER_DAY_FRI,
                              StrId::STR_WEATHER_DAY_SAT};

  const char* unitSuffix = WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
  int numDays = static_cast<int>(weatherData.daily.size());

  if (numDays <= 0) return;

  // Derive weekday progression from the first forecast day to avoid timezone
  // conversion edge cases causing repeated day labels.
  struct tm firstDayInfo;
  time_t firstLocalDate = weatherData.daily[0].date + weatherData.utcOffsetSeconds;
  gmtime_r(&firstLocalDate, &firstDayInfo);
  const int firstWeekday = firstDayInfo.tm_wday;

  // Keep all day panels evenly split (distribute remainder 1px left-to-right)
  const int baseDayWidth = w / numDays;
  const int extraPx = w % numDays;

  int cardX = x;

  for (int i = 0; i < numDays; i++) {
    const auto& day = weatherData.daily[i];
    int cardWidth = baseDayWidth + (i < extraPx ? 1 : 0);
    int textY = y + 10;

    // Day name (localized)
    struct tm timeinfo;
    time_t localDate = day.date + weatherData.utcOffsetSeconds;
    gmtime_r(&localDate, &timeinfo);
    const int weekdayIndex = (firstWeekday + i) % 7;
    const char* dayName = I18N.get(dayNameIds[weekdayIndex]);

    int dayNameWidth = renderer.getTextWidth(UI_10_FONT_ID, dayName, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, cardX + (cardWidth - dayNameWidth) / 2, textY, dayName, true, EpdFontFamily::BOLD);
    textY += 20;

    // Date (e.g. "Apr 3") - for all days
    char dateBuf[16];
    const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(dateBuf, sizeof(dateBuf), "%s %d", monthNames[timeinfo.tm_mon], timeinfo.tm_mday);
    int dateWidth = renderer.getTextWidth(SMALL_FONT_ID, dateBuf);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - dateWidth) / 2, textY, dateBuf);
    textY += 18;

    // Weather icon (WEATHER_ICON_LARGE for all days)
    auto iconType = getWeatherIconType(day.weatherCode, true);
    const uint8_t* icon = getWeatherIconLarge(iconType);
    int iconX = cardX + (cardWidth - WEATHER_ICON_LARGE) / 2;
    drawWeatherIconWithOrientation(renderer, icon, iconX, textY, WEATHER_ICON_LARGE);
    textY += WEATHER_ICON_LARGE + 8;

    // Weather description (short)
    const char* desc = I18N.get(getWeatherDescriptionStrId(day.weatherCode));
    auto truncDesc = renderer.truncatedText(SMALL_FONT_ID, desc, cardWidth - 8);
    int descWidth = renderer.getTextWidth(SMALL_FONT_ID, truncDesc.c_str());
    renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - descWidth) / 2, textY, truncDesc.c_str());
    textY += 16;

    // High / Low temp with unit
    char tempBuf[32];
    snprintf(tempBuf, sizeof(tempBuf), "%.0f / %.0f %s", day.tempMax, day.tempMin, unitSuffix);
    int tempWidth = renderer.getTextWidth(SMALL_FONT_ID, tempBuf, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - tempWidth) / 2, textY, tempBuf, true, EpdFontFamily::BOLD);
    textY += 16;

    // Precipitation
    char precipBuf[24];
    if (std::fabs(day.precipSum) < 0.0001f) {
      snprintf(precipBuf, sizeof(precipBuf), "%s: --", tr(STR_WEATHER_PRECIP));
    } else {
      snprintf(precipBuf, sizeof(precipBuf), "%s: %.1f %s", tr(STR_WEATHER_PRECIP), day.precipSum,
               WEATHER_SETTINGS.getPrecipUnitParam());
    }
    auto truncPrecip = renderer.truncatedText(SMALL_FONT_ID, precipBuf, cardWidth - 8);
    int precipWidth = renderer.getTextWidth(SMALL_FONT_ID, truncPrecip.c_str());
    renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - precipWidth) / 2, textY, truncPrecip.c_str());
    textY += 16;

    // UV Index
    char uvBuf[16];
    snprintf(uvBuf, sizeof(uvBuf), "UV: %.0f", day.uvIndexMax);
    int uvWidth = renderer.getTextWidth(SMALL_FONT_ID, uvBuf);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - uvWidth) / 2, textY, uvBuf);

    // Card separator
    if (i < numDays - 1) {
      renderer.drawLine(cardX + cardWidth, y, cardX + cardWidth, y + h);
    }

    cardX += cardWidth;
  }
}

void WeatherActivity::renderHourlyGraph(int x, int y, int w, int h) {
  if (weatherData.hourly.empty()) return;

  // Localized weekday names (indexed by tm_wday: 0=Sun..6=Sat)
  const StrId dayNameIds[] = {StrId::STR_WEATHER_DAY_SUN, StrId::STR_WEATHER_DAY_MON, StrId::STR_WEATHER_DAY_TUE,
                              StrId::STR_WEATHER_DAY_WED, StrId::STR_WEATHER_DAY_THU, StrId::STR_WEATHER_DAY_FRI,
                              StrId::STR_WEATHER_DAY_SAT};

  constexpr int leftMargin = 56;  // Space for Y-axis labels with unit suffix
  constexpr int rightMargin = 10;
  constexpr int topMargin = 28;     // Keep heading clear of top temperature label
  constexpr int bottomMargin = 25;  // Space for X-axis labels
  constexpr int precipBarMaxHeight = 30;

  const int graphX = x + leftMargin;
  const int graphY = y + topMargin;
  const int graphW = w - leftMargin - rightMargin;
  const int graphH = h - topMargin - bottomMargin - precipBarMaxHeight;
  const int precipY = graphY + graphH;

  size_t numPoints = weatherData.hourly.size();
  if (numPoints < 2) return;

  // Find day boundaries in hourly data (start index of each day).
  std::vector<size_t> dayStartIndices;
  dayStartIndices.reserve(8);
  dayStartIndices.push_back(0);
  struct tm prevDayInfo;
  time_t prevLocalTime = weatherData.hourly[0].time + weatherData.utcOffsetSeconds;
  gmtime_r(&prevLocalTime, &prevDayInfo);
  for (size_t i = 1; i < numPoints; i++) {
    struct tm curDayInfo;
    time_t localTime = weatherData.hourly[i].time + weatherData.utcOffsetSeconds;
    gmtime_r(&localTime, &curDayInfo);
    if (curDayInfo.tm_year != prevDayInfo.tm_year || curDayInfo.tm_yday != prevDayInfo.tm_yday) {
      dayStartIndices.push_back(i);
      prevDayInfo = curDayInfo;
    }
  }

  // Find temperature range
  float tempMin = weatherData.hourly[0].temperature;
  float tempMax = tempMin;
  float maxPrecip = 0;
  for (const auto& hr : weatherData.hourly) {
    tempMin = std::min(tempMin, hr.temperature);
    tempMax = std::max(tempMax, hr.temperature);
    maxPrecip = std::max(maxPrecip, hr.precipitation);
  }

  // Add padding to temp range
  float tempRange = tempMax - tempMin;
  if (tempRange < 5.0f) {
    float mid = (tempMax + tempMin) / 2.0f;
    tempMin = mid - 2.5f;
    tempMax = mid + 2.5f;
    tempRange = 5.0f;
  }
  tempMin -= 1.0f;
  tempMax += 1.0f;
  tempRange = tempMax - tempMin;

  // Draw graph title centered over the graph area.
  const char* graphTitle = tr(STR_WEATHER_48H_FORECAST);
  const int graphTitleWidth = renderer.getTextWidth(SMALL_FONT_ID, graphTitle, EpdFontFamily::BOLD);
  const int graphTitleX = graphX + (graphW - graphTitleWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, graphTitleX, y + 3, graphTitle, true, EpdFontFamily::BOLD);

  // Draw weekday labels above the chart area at each day start.
  for (size_t startIdx : dayStartIndices) {
    struct tm dayInfo;
    time_t localTime = weatherData.hourly[startIdx].time + weatherData.utcOffsetSeconds;
    gmtime_r(&localTime, &dayInfo);

    const char* dayName = I18N.get(dayNameIds[dayInfo.tm_wday]);
    const int dayLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, dayName, EpdFontFamily::BOLD);
    const int px = graphX + static_cast<int>(startIdx * graphW / (numPoints - 1));
    int labelX = px + 2;
    const int maxLabelX = graphX + graphW - dayLabelWidth - 2;
    if (labelX > maxLabelX) {
      labelX = maxLabelX;
    }
    if (labelX < graphX + 2) {
      labelX = graphX + 2;
    }
    renderer.drawText(SMALL_FONT_ID, labelX, y + 14, dayName, true, EpdFontFamily::BOLD);
  }

  // Draw Y-axis labels (temperature)
  float tempStep = tempRange / 4.0f;
  const char* unitSuffix = WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
  for (int i = 0; i <= 4; i++) {
    float temp = tempMax - i * tempStep;
    int labelY = graphY + i * graphH / 4;
    char label[16];
    snprintf(label, sizeof(label), "%.0f %s", temp, unitSuffix);
    renderer.drawText(SMALL_FONT_ID, x + 2, labelY - 4, label);
    // Grid line (dashed effect using short segments)
    for (int gx = graphX; gx < graphX + graphW; gx += 8) {
      renderer.drawPixel(gx, labelY);
    }
  }

  // Draw temperature line
  int prevPx = -1, prevPy = -1;
  for (size_t i = 0; i < numPoints; i++) {
    int px = graphX + static_cast<int>(i * graphW / (numPoints - 1));
    float normalized = (weatherData.hourly[i].temperature - tempMin) / tempRange;
    int py = graphY + graphH - static_cast<int>(normalized * graphH);

    if (prevPx >= 0) {
      renderer.drawLine(prevPx, prevPy, px, py, 2, true);
    }
    prevPx = px;
    prevPy = py;
  }

  // Draw precipitation bars
  if (maxPrecip > 0) {
    for (size_t i = 0; i < numPoints; i++) {
      float precip = weatherData.hourly[i].precipitation;
      if (precip <= 0) continue;

      int px = graphX + static_cast<int>(i * graphW / (numPoints - 1));
      int barH = static_cast<int>((precip / maxPrecip) * precipBarMaxHeight);
      if (barH < 1) barH = 1;
      int barW = std::max(1, graphW / static_cast<int>(numPoints) - 1);
      renderer.fillRect(px - barW / 2, precipY + precipBarMaxHeight - barH, barW, barH);
    }
  }

  // Draw day separator lines for day boundaries after the first day.
  for (size_t boundaryIdx = 1; boundaryIdx < dayStartIndices.size(); boundaryIdx++) {
    const size_t i = dayStartIndices[boundaryIdx];
    const int px = graphX + static_cast<int>(i * graphW / (numPoints - 1));
    renderer.drawLine(px, graphY, px, precipY + precipBarMaxHeight);
  }

  // Draw X-axis labels (every 6 hours)
  for (size_t i = 0; i < numPoints; i += 6) {
    int px = graphX + static_cast<int>(i * graphW / (numPoints - 1));
    struct tm timeinfo;
    time_t localTime = weatherData.hourly[i].time + weatherData.utcOffsetSeconds;
    gmtime_r(&localTime, &timeinfo);
    char label[8];
    snprintf(label, sizeof(label), "%02d:00", timeinfo.tm_hour);
    renderer.drawText(SMALL_FONT_ID, px - 12, precipY + precipBarMaxHeight + 3, label);

    // Vertical grid line
    for (int gy = graphY; gy < precipY + precipBarMaxHeight; gy += 8) {
      renderer.drawPixel(px, gy);
    }
  }

  // Draw axes
  renderer.drawLine(graphX, graphY, graphX, precipY + precipBarMaxHeight);  // Y axis
  renderer.drawLine(graphX, precipY + precipBarMaxHeight, graphX + graphW,
                    precipY + precipBarMaxHeight);  // X axis
}
