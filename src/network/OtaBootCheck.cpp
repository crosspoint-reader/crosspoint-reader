#include "OtaBootCheck.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "fontIds.h"

namespace {

struct RtcRequest {
  uint32_t magic;
  uint8_t stage;
  char version[32];
  char url[512];
  uint32_t size;
};

constexpr uint32_t OTA_BOOT_MAGIC = 0xC7055074;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

// Survives ESP.restart() but not power loss; a lost request simply lands the
// user back on the OTA screen, which re-requests.
RTC_NOINIT_ATTR RtcRequest rtcRequest;

OtaBootCheck::Result bootResult;
bool bootResultValid = false;

void drawStatus(GfxRenderer& renderer, const char* text) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, text);
  renderer.displayBuffer();
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

bool connectSavedWifi() {
  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (cred == nullptr) {
    LOG_ERR("OTA", "Boot stage: no saved WiFi credential");
    return false;
  }

  LOG_INF("OTA", "Boot stage: connecting to %s (heap=%u maxAlloc=%u)", cred->ssid.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("OTA", "Boot stage: WiFi connect timed out (ssid=%s)", cred->ssid.c_str());
    return false;
  }
  LOG_INF("OTA", "Boot stage: connected, ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void runCheckStage(OtaUpdater& updater) {
  bootResult.error = updater.checkForUpdate();
  if (bootResult.error != OtaUpdater::OK) return;

  strncpy(bootResult.version, updater.getLatestVersion().c_str(), sizeof(bootResult.version) - 1);
  strncpy(bootResult.url, updater.getOtaUrl().c_str(), sizeof(bootResult.url) - 1);
  bootResult.size = static_cast<uint32_t>(updater.getOtaSize());
}

void runInstallStage(OtaUpdater& updater, GfxRenderer& renderer) {
  // RTC memory is not trusted blindly: terminate the strings even though
  // requestInstall wrote them, in case of partial corruption across the reset.
  rtcRequest.version[sizeof(rtcRequest.version) - 1] = '\0';
  rtcRequest.url[sizeof(rtcRequest.url) - 1] = '\0';
  updater.adoptManifest(rtcRequest.version, rtcRequest.url, rtcRequest.size);

  drawStatus(renderer, tr(STR_UPDATING));

  // The panel retains the frame just drawn, so hand the framebuffer heap to
  // the download: the TLS pipeline plus flash writes need every contiguous
  // block available. No progress UI — rendering is offline until the buffers
  // come back below.
  display.releaseBuffers();
  bootResult.error = updater.installUpdate(nullptr, nullptr);
  const bool buffersBack = display.reallocBuffers();
  if (!buffersBack) {
    // Can't render anything; reboot clean. The pending result is lost and the
    // OTA screen will simply offer the update again.
    LOG_ERR("OTA", "Framebuffer realloc failed after install, restarting");
    ESP.restart();
  }

  if (bootResult.error != OtaUpdater::OK) return;

  drawStatus(renderer, tr(STR_UPDATE_COMPLETE));
  delay(3000);
  ESP.restart();  // boots the freshly written partition
}

}  // namespace

namespace OtaBootCheck {

Stage takeStage() {
  if (rtcRequest.magic != OTA_BOOT_MAGIC) return Stage::None;
  rtcRequest.magic = 0;  // read-and-clear: a panic in the stage must not loop the boot
  switch (rtcRequest.stage) {
    case static_cast<uint8_t>(Stage::Check):
      return Stage::Check;
    case static_cast<uint8_t>(Stage::Install):
      return Stage::Install;
    default:
      return Stage::None;
  }
}

void runStage(const Stage stage, GfxRenderer& renderer) {
  bootResult = Result{};
  bootResultValid = true;

  LOG_INF("OTA", "Boot stage %d starting (heap=%u maxAlloc=%u)", static_cast<int>(stage), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  drawStatus(renderer, tr(STR_CHECKING_UPDATE));

  if (!connectSavedWifi()) {
    bootResult.error = OtaUpdater::HTTP_ERROR;
    wifiOff();
    return;
  }

  OtaUpdater updater;
  if (stage == Stage::Check) {
    runCheckStage(updater);
  } else {
    runInstallStage(updater, renderer);  // does not return on success
  }

  wifiOff();
  LOG_INF("OTA", "Boot stage done: error=%d version=%s", static_cast<int>(bootResult.error), bootResult.version);
}

const Result* takeResult() {
  if (!bootResultValid) return nullptr;
  bootResultValid = false;
  return &bootResult;
}

bool canAutoConnect() {
  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  return !lastSsid.empty() && WIFI_STORE.findCredential(lastSsid) != nullptr;
}

void requestCheck() {
  memset(&rtcRequest, 0, sizeof(rtcRequest));
  rtcRequest.stage = static_cast<uint8_t>(Stage::Check);
  rtcRequest.magic = OTA_BOOT_MAGIC;
  LOG_INF("OTA", "Restarting into boot-time update check");
  silentRestart();
}

void requestInstall(const char* version, const char* url, const size_t size) {
  memset(&rtcRequest, 0, sizeof(rtcRequest));
  rtcRequest.stage = static_cast<uint8_t>(Stage::Install);
  if (version != nullptr) strncpy(rtcRequest.version, version, sizeof(rtcRequest.version) - 1);
  if (url != nullptr) strncpy(rtcRequest.url, url, sizeof(rtcRequest.url) - 1);
  rtcRequest.size = static_cast<uint32_t>(size);
  rtcRequest.magic = OTA_BOOT_MAGIC;
  LOG_INF("OTA", "Restarting into boot-time install of %s", rtcRequest.version);
  silentRestart();
}

}  // namespace OtaBootCheck
