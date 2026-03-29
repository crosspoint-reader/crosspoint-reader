#include "DetectTimezoneActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
bool mapIanaTimezone(const std::string& tz, uint8_t& outSetting) {
  using TZ = CrossPointSettings::TIMEZONE;
  if (tz == "UTC" || tz == "Etc/UTC") {
    outSetting = TZ::TZ_UTC;
    return true;
  }
  if (tz == "Europe/London" || tz == "Europe/Guernsey" || tz == "Europe/Isle_of_Man" || tz == "Europe/Jersey") {
    outSetting = TZ::TZ_UTC;
    return true;
  }
  if (tz == "Europe/Athens" || tz == "Europe/Bucharest" || tz == "Europe/Helsinki" || tz == "Europe/Kiev" ||
      tz == "Europe/Vilnius" || tz == "Europe/Riga" || tz == "Europe/Tallinn") {
    outSetting = TZ::TZ_EET;
    return true;
  }
  if (tz == "Europe/Moscow") {
    outSetting = TZ::TZ_MSK;
    return true;
  }
  if (tz.rfind("Europe/", 0) == 0) {
    outSetting = TZ::TZ_CET;
    return true;
  }
  if (tz == "America/New_York" || tz == "America/Toronto") {
    outSetting = TZ::TZ_EST;
    return true;
  }
  if (tz == "America/Chicago") {
    outSetting = TZ::TZ_CST;
    return true;
  }
  if (tz == "America/Denver") {
    outSetting = TZ::TZ_MST;
    return true;
  }
  if (tz == "America/Los_Angeles" || tz == "America/Vancouver") {
    outSetting = TZ::TZ_PST;
    return true;
  }
  if (tz == "America/Sao_Paulo" || tz == "America/Argentina/Buenos_Aires" || tz == "America/Montevideo") {
    outSetting = TZ::TZ_UTC_MINUS3;
    return true;
  }
  if (tz == "Asia/Dubai" || tz == "Asia/Muscat") {
    outSetting = TZ::TZ_UTC_PLUS4;
    return true;
  }
  if (tz == "Asia/Kolkata") {
    outSetting = TZ::TZ_IST;
    return true;
  }
  if (tz == "Asia/Bangkok" || tz == "Asia/Ho_Chi_Minh" || tz == "Asia/Jakarta" || tz == "Asia/Phnom_Penh" ||
      tz == "Asia/Vientiane") {
    outSetting = TZ::TZ_UTC_PLUS7;
    return true;
  }
  if (tz == "Asia/Shanghai" || tz == "Asia/Hong_Kong" || tz == "Asia/Singapore" || tz == "Asia/Taipei" ||
      tz == "Asia/Kuala_Lumpur" || tz == "Asia/Manila") {
    outSetting = TZ::TZ_UTC_PLUS8;
    return true;
  }
  if (tz == "Asia/Tokyo" || tz == "Asia/Seoul") {
    outSetting = TZ::TZ_UTC_PLUS9;
    return true;
  }
  if (tz == "Australia/Sydney" || tz == "Australia/Melbourne" || tz == "Australia/Hobart") {
    outSetting = TZ::TZ_AEST;
    return true;
  }
  if (tz == "Pacific/Auckland") {
    outSetting = TZ::TZ_NZST;
    return true;
  }
  return false;
}

bool fetchTimezonePayload(const char* url, std::string& payload) {
  // Give DNS a moment after WiFi connect.
  delay(500);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (HttpDownloader::fetchUrl(url, payload)) {
      return true;
    }
    delay(300);
  }
  return false;
}

bool fetchPublicIp(std::string& ip) {
  std::string payload;
  if (!fetchTimezonePayload("https://api.ipify.org", payload)) {
    return false;
  }

  // Payload is plain text: IP address
  ip = payload;
  // Trim any whitespace
  while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ' || ip.back() == '\t')) {
    ip.pop_back();
  }
  if (!ip.empty()) {
    LOG_DBG("CLK", "Public IP: %s", ip.c_str());
    return true;
  }
  return false;
}

bool detectTimezoneSetting(uint8_t& outSetting, std::string& outIana, bool& outDstKnown, bool& outDstActive) {
  std::string payload;
  std::string publicIp;
  if (fetchPublicIp(publicIp)) {
    std::string timeUrl = std::string("https://timeapi.io/api/Time/current/ip?ipAddress=") + publicIp;
    LOG_DBG("CLK", "Timezone detect via TimeAPI: %s", timeUrl.c_str());
    if (fetchTimezonePayload(timeUrl.c_str(), payload)) {
      // Continue to parse TimeAPI payload below.
    }
  }

  if (payload.empty() && !fetchTimezonePayload("https://ip-api.com/json/?fields=timezone,dst", payload)) {
    LOG_ERR("CLK", "Timezone detect failed: fetch error");
    return false;
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, payload);
  if (err) {
    LOG_ERR("CLK", "Timezone detect failed: %s", err.c_str());
    return false;
  }

  const char* tz = doc["timezone"] | "";
  if (!tz || tz[0] == '\0') {
    tz = doc["timeZone"] | "";
  }
  if (!tz || tz[0] == '\0') {
    tz = doc["time_zone"] | "";
  }
  if (!tz || tz[0] == '\0') {
    LOG_ERR("CLK", "Timezone detect failed: missing timezone (payload: %s)", payload.c_str());
    return false;
  }

  outIana = tz;
  if (!doc["dst_active"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dst_active"].as<bool>();
  } else if (!doc["dstActive"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dstActive"].as<bool>();
  } else if (!doc["dst"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dst"].as<bool>();
  } else {
    outDstKnown = false;
    outDstActive = false;
  }

  if (!mapIanaTimezone(tz, outSetting)) {
    LOG_ERR("CLK", "Timezone detect unsupported: %s", tz);
    return false;
  }

  if (outDstKnown) {
    LOG_DBG("CLK", "Timezone detected: %s (dst=%d)", tz, outDstActive ? 1 : 0);
  } else {
    LOG_DBG("CLK", "Timezone detected: %s (dst=unknown)", tz);
  }
  return true;
}

}  // namespace

void DetectTimezoneActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onWifiSelectionCancelled();
                             return;
                           }
                           onWifiSelectionComplete(true);
                         });
}

void DetectTimezoneActivity::onExit() {
  Activity::onExit();
  HalClock::wifiOff();
}

void DetectTimezoneActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = FAILED;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = DETECTING;
  }
  requestUpdateAndWait();

  performDetect();
}

void DetectTimezoneActivity::onWifiSelectionCancelled() { finish(); }

void DetectTimezoneActivity::performDetect() {
  uint8_t detected = SETTINGS.timeZone;
  detectedTimezone.clear();
  dstKnown = false;
  dstActive = false;
  if (detectTimezoneSetting(detected, detectedTimezone, dstKnown, dstActive)) {
    SETTINGS.timeZone = detected;
    HalClock::applyTimezone(SETTINGS.timeZone);
    SETTINGS.saveToFile();
    state = SUCCESS;
  } else {
    state = FAILED;
  }

  HalClock::wifiOff();
  requestUpdate();
}

void DetectTimezoneActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DETECT_TIMEZONE));

  if (state == CONNECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CONNECTING), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == DETECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_DETECTING_TIMEZONE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_TIMEZONE_DETECTED), true, EpdFontFamily::BOLD);

    if (!detectedTimezone.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, detectedTimezone.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, dstStatusLabel());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_TIMEZONE_DETECT_FAILED), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

const char* DetectTimezoneActivity::dstStatusLabel() const {
  if (!dstKnown) {
    return tr(STR_DST_UNKNOWN);
  }
  return dstActive ? tr(STR_DST_ACTIVE) : tr(STR_DST_INACTIVE);
}

void DetectTimezoneActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
