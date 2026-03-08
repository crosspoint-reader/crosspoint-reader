#include <Arduino.h>
#include <Epub.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <Update.h>
#include <bootloader_common.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "../.pio/libdeps/default/SdFat/src/common/FsDateTime.h"  // FsDateTime::setCallback

// Direct OTA data partition write — bypasses esp_ota_set_boot_partition()'s image
// validation, which fails for Arduino/unsigned builds lacking an embedded SHA256.
// Replicates the same otadata format as crosspoint-flash.py's make_entry().
static esp_err_t forceSetBootPartition(const esp_partition_t* newPart) {
  if (!newPart) return ESP_ERR_INVALID_ARG;

  const esp_partition_t* otaPart =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otaPart) return ESP_ERR_NOT_FOUND;

  // OTA select entry: seq(4) + label(20) + state(4) + crc(4) = 32 bytes
  static constexpr size_t ENTRY_SIZE = 32;
  static constexpr size_t SECTOR_SIZE = 0x1000;

  struct __attribute__((packed)) OtaEntry {
    uint32_t seq;
    uint8_t label[20];
    uint32_t state;
    uint32_t crc;
  };
  static_assert(sizeof(OtaEntry) == ENTRY_SIZE, "");

  OtaEntry e0, e1;
  memset(&e0, 0xFF, sizeof(e0));
  memset(&e1, 0xFF, sizeof(e1));
  esp_partition_read(otaPart, 0, &e0, sizeof(e0));
  esp_partition_read(otaPart, SECTOR_SIZE, &e1, sizeof(e1));

  uint32_t seq0 = (e0.seq == 0xFFFFFFFF) ? 0 : e0.seq;
  uint32_t seq1 = (e1.seq == 0xFFFFFFFF) ? 0 : e1.seq;
  uint32_t maxSeq = (seq0 > seq1) ? seq0 : seq1;

  // Determine partition index (ota_0=0, ota_1=1) and number of OTA slots
  uint32_t partIdx = newPart->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  uint32_t numOta = 2;  // standard 2-slot layout

  // New seq must be > maxSeq and satisfy: (seq - 1) % numOta == partIdx
  uint32_t newSeq = maxSeq + 1;
  while (((newSeq - 1) % numOta) != partIdx) newSeq++;

  // Ping-pong: overwrite the sector with the OLDER (lower) sequence so that
  // the other sector — which holds the current highest-seq entry — remains as
  // a valid fallback if this write is interrupted.
  bool writeToSector1 = (seq1 <= seq0);
  uint32_t writeOffset = writeToSector1 ? SECTOR_SIZE : 0;

  // Build the 32-byte entry matching esp_ota_select_entry_t layout
  OtaEntry entry;
  entry.seq = newSeq;
  memset(entry.label, 0xFF, sizeof(entry.label));
  // When called from setup() (self-confirm), write VALID so the bootloader
  // never rolls back.  When called during OTA, the caller passes the target
  // partition — PENDING_VERIFY would be correct but VALID also works since
  // the firmware will call forceSetBootPartition(self) on next boot anyway.
  entry.state = ESP_OTA_IMG_VALID;
  // Use the official bootloader CRC function — guaranteed to match validation.
  // CRC covers ota_seq field only (4 bytes), per esp_flash_partitions.h comment.
  entry.crc = bootloader_common_ota_select_crc(reinterpret_cast<const esp_ota_select_entry_t*>(&entry));

  LOG_INF("OTA", "forceSetBootPartition: part=%s(@0x%lx) idx=%lu seq0=%lu seq1=%lu → newSeq=%lu sector=%lu",
          newPart->label, (unsigned long)newPart->address, (unsigned long)partIdx, (unsigned long)seq0,
          (unsigned long)seq1, (unsigned long)newSeq, (unsigned long)(writeOffset / SECTOR_SIZE));

  esp_err_t err = esp_partition_erase_range(otaPart, writeOffset, SECTOR_SIZE);
  if (err != ESP_OK) return err;
  return esp_partition_write(otaPart, writeOffset, &entry, sizeof(entry));
}

#include <ESPmDNS.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointWebServer.h"
#include "network/RssFeedSync.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenCapture.h"
#include "util/ScreenshotUtil.h"

// Expose the compile-time version string to other translation units (e.g. PulsrTheme).
extern "C" const char* getVersionString() { return CROSSPOINT_VERSION; }

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);

// Danger Zone: background web server (lives outside activity lifecycle)
static std::unique_ptr<CrossPointWebServer> dzWebServer;
static bool dzWifiConnected = false;
volatile bool dzScreenshotTourRequested = false;
volatile bool dzFlashRequested = false;
FontDecompressor fontDecompressor;

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont opendyslexic8RegularFont(&opendyslexic_8_regular);
EpdFont opendyslexic8BoldFont(&opendyslexic_8_bold);
EpdFont opendyslexic8ItalicFont(&opendyslexic_8_italic);
EpdFont opendyslexic8BoldItalicFont(&opendyslexic_8_bolditalic);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldItalicFont);
EpdFont opendyslexic10RegularFont(&opendyslexic_10_regular);
EpdFont opendyslexic10BoldFont(&opendyslexic_10_bold);
EpdFont opendyslexic10ItalicFont(&opendyslexic_10_italic);
EpdFont opendyslexic10BoldItalicFont(&opendyslexic_10_bolditalic);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldItalicFont);
EpdFont opendyslexic12RegularFont(&opendyslexic_12_regular);
EpdFont opendyslexic12BoldFont(&opendyslexic_12_bold);
EpdFont opendyslexic12ItalicFont(&opendyslexic_12_italic);
EpdFont opendyslexic12BoldItalicFont(&opendyslexic_12_bolditalic);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldItalicFont);
EpdFont opendyslexic14RegularFont(&opendyslexic_14_regular);
EpdFont opendyslexic14BoldFont(&opendyslexic_14_bold);
EpdFont opendyslexic14ItalicFont(&opendyslexic_14_italic);
EpdFont opendyslexic14BoldItalicFont(&opendyslexic_14_bolditalic);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldItalicFont);
#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

EpdFont pulsr10Font(&antonio_10_regular);
EpdFontFamily pulsr10FontFamily(&pulsr10Font);

EpdFont pulsr12Font(&antonio_12_regular);
EpdFontFamily pulsr12FontFamily(&pulsr12Font);
// Antonio reading sizes (Regular only)
EpdFont antonio12Font(&antonio_12_regular);
EpdFontFamily antonio12FontFamily(&antonio12Font);
EpdFont antonio14Font(&antonio_14_regular);
EpdFontFamily antonio14FontFamily(&antonio14Font);
EpdFont antonio16Font(&antonio_16_regular);
EpdFontFamily antonio16FontFamily(&antonio16Font);
EpdFont antonio18Font(&antonio_18_regular);
EpdFontFamily antonio18FontFamily(&antonio18Font);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Tear down Danger Zone web server and WiFi, freeing heap for reading.
// Safe to call even if DZ is not active.
// If disableFlag=true, also clears dangerZoneEnabled so it won't auto-reconnect
// on the next wake (used for inactivity-triggered sleep).
void teardownDangerZone(const char* reason = "reading", bool disableFlag = false) {
  if (dzWebServer || dzWifiConnected) {
    LOG_INF("DZ", "Tearing down WiFi + web server (%s)", reason);
    if (dzWebServer) {
      dzWebServer->stop();
      dzWebServer.reset();
    }
    dzWifiConnected = false;
    UITheme::setHttpServerActive(false);
    UITheme::setNetworkStatus(false, false);
    WiFi.disconnect(true);  // true = turn off radio
    WiFi.mode(WIFI_OFF);
    LOG_INF("DZ", "Teardown complete. Free heap: %lu", esp_get_free_heap_size());
  }
  if (disableFlag && SETTINGS.dangerZoneEnabled) {
    LOG_INF("DZ", "Auto-disabling Danger Zone due to inactivity timeout");
    SETTINGS.dangerZoneEnabled = false;
    SETTINGS.saveToFile();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation

  // Note: teardownDangerZone is NOT called here. It is called explicitly by
  // the inactivity-timeout path before enterDeepSleep(). The manual sleep
  // button leaves DZ running so it stays accessible after wake.

  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  APP_STATE.saveToFile();

  activityManager.goToSleep();

  display.deepSleep();
  LOG_DBG("MAIN", "Power button press calibration value: %lu ms", t2 - t1);
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  activityManager.begin();
  // Tear down WiFi/web server before entering the reader to free heap for EPUB parsing.
  activityManager.beforeOpenReader = []() { teardownDangerZone("opening reader"); };
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  renderer.setFontDecompressor(&fontDecompressor);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID, opendyslexic8FontFamily);
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID, opendyslexic10FontFamily);
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID, opendyslexic12FontFamily);
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID, opendyslexic14FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(PULSR_10_FONT_ID, pulsr10FontFamily);
  renderer.insertFont(PULSR_12_FONT_ID, pulsr12FontFamily);
  // Antonio reading font sizes
  renderer.insertFont(ANTONIO_12_FONT_ID, antonio12FontFamily);
  renderer.insertFont(ANTONIO_14_FONT_ID, antonio14FontFamily);
  renderer.insertFont(ANTONIO_16_FONT_ID, antonio16FontFamily);
  renderer.insertFont(ANTONIO_18_FONT_ID, antonio18FontFamily);
  LOG_DBG("MAIN", "Fonts setup");
}

// Reconnect WiFi using saved credentials — called after screenshot tour to restore connectivity
// regardless of Danger Zone state (so the user isn't left offline after the tour).
static void reconnectWifiAfterTour() {
  WIFI_STORE.loadFromFile();
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("SCR", "No last connected SSID — skipping WiFi restore");
    return;
  }
  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_DBG("SCR", "No saved password for '%s' — skipping WiFi restore", lastSsid.c_str());
    return;
  }
  LOG_INF("SCR", "Restoring WiFi to '%s' after screenshot tour...", lastSsid.c_str());
  UITheme::setWifiAutoConnecting(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  constexpr unsigned long TIMEOUT_MS = 15000;
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < TIMEOUT_MS) {
    delay(100);
  }
  UITheme::setWifiAutoConnecting(false);
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("SCR", "WiFi restore failed after tour (status=%d)", WiFi.status());
    WiFi.mode(WIFI_OFF);
  } else {
    LOG_INF("SCR", "WiFi restored! IP=%s", WiFi.localIP().toString().c_str());
  }
}

// Danger Zone: attempt auto-connect to last known WiFi, start web server + feed sync.
// Non-blocking: returns quickly regardless of outcome. Logs result.
void dangerZoneAutoConnect() {
  if (!SETTINGS.dangerZoneEnabled) return;
  if (SETTINGS.dangerZonePassword[0] == '\0') {
    LOG_DBG("DZ", "Danger Zone enabled but no password set — skipping auto-connect");
    return;
  }

  WIFI_STORE.loadFromFile();
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("DZ", "No last connected SSID — skipping auto-connect");
    return;
  }

  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_DBG("DZ", "No saved password for '%s' — skipping", lastSsid.c_str());
    return;
  }

  LOG_INF("DZ", "Auto-connecting to '%s'...", lastSsid.c_str());
  UITheme::setWifiAutoConnecting(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

  // Block up to 15 seconds for connection
  constexpr unsigned long TIMEOUT_MS = 15000;
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < TIMEOUT_MS) {
    delay(100);
  }

  UITheme::setWifiAutoConnecting(false);

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("DZ", "Auto-connect failed (status=%d)", WiFi.status());
    WiFi.mode(WIFI_OFF);
    return;
  }

  dzWifiConnected = true;
  LOG_INF("DZ", "Connected! IP=%s", WiFi.localIP().toString().c_str());

  configTime(0, 0, "pool.ntp.org");
  {
    time_t t = 0;
    int tries = 0;
    while (time(&t) < 1000000000L && tries++ < 30) delay(100);
  }
  FsDateTime::setCallback([](uint16_t* date, uint16_t* tv) {
    time_t now = ::time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    *date = FS_DATE(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    *tv = FS_TIME(tm.tm_hour, tm.tm_min, tm.tm_sec);
  });

  UITheme::setNetworkStatus(true, false);

  // Start mDNS
  MDNS.begin("crosspoint");

  // Start web server
  dzWebServer.reset(new CrossPointWebServer());
  dzWebServer->begin();
  if (dzWebServer->isRunning()) {
    UITheme::setHttpServerActive(true);
    LOG_INF("DZ", "Web server started on port 80");
  } else {
    LOG_ERR("DZ", "Web server failed to start");
    dzWebServer.reset();
  }

  // Start RSS feed sync — skip if Up or Down is held at connect time
  gpio.update();
  if (gpio.isPressed(HalGPIO::BTN_UP) || gpio.isPressed(HalGPIO::BTN_DOWN)) {
    LOG_INF("DZ", "Button held at WiFi connect — suppressing RSS feed sync");
    RssFeedSync::suppressSync();
  }
  RssFeedSync::startSync();
}

void setup() {
  // MUST be first — confirms the running firmware is healthy.
  // Bypasses esp_ota_mark_app_valid_cancel_rollback() because that function
  // calls image_validate() internally, which FAILS for unsigned Arduino builds
  // (no embedded SHA256) and then ABORTS the firmware + reboots.  Instead,
  // write state=VALID directly into the otadata for the running partition.
  {
    const esp_partition_t* self = esp_ota_get_running_partition();
    if (self) forceSetBootPartition(self);
  }

  t1 = millis();

  HalSystem::begin();
  gpio.begin();
  powerManager.begin();

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 3 seconds for Serial to be ready to catch early logs
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) {
      delay(10);
    }
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();
  HalSystem::clearPanic();  // TODO: move this to an activity when we have one to display the panic info

  SETTINGS.loadFromFile();
  SETTINGS.clampToValidRanges();
  I18N.loadSettings();

  // Boot log: write early so any subsequent crash is detectable
  {
    const esp_reset_reason_t resetReason = esp_reset_reason();
    const char* resetStr = (resetReason == ESP_RST_PANIC)       ? "panic"
                           : (resetReason == ESP_RST_INT_WDT)   ? "int_wdt"
                           : (resetReason == ESP_RST_TASK_WDT)  ? "task_wdt"
                           : (resetReason == ESP_RST_WDT)       ? "wdt"
                           : (resetReason == ESP_RST_BROWNOUT)  ? "brownout"
                           : (resetReason == ESP_RST_SW)        ? "sw"
                           : (resetReason == ESP_RST_POWERON)   ? "poweron"
                           : (resetReason == ESP_RST_DEEPSLEEP) ? "deepsleep"
                                                                : "other";
    Storage.mkdir("/.crosspoint");  // ensure hidden dir exists before writing logs

    // Write current firmware version to hidden file (for scripts, feed server, etc.)
    // Also remove legacy /firmware.version from root if it exists.
    Storage.remove("/firmware.version");
    {
      FsFile vf = Storage.open("/.crosspoint/version.txt", O_WRONLY | O_CREAT | O_TRUNC);
      if (vf) {
        vf.println(CROSSPOINT_VERSION);
        vf.close();
      }
    }

    FsFile bootLog;
    // Rotate log if over 2KB
    {
      FsFile check = Storage.open("/.crosspoint/boot.log");
      if (check && check.size() > 2048) {
        check.close();
        Storage.remove("/.crosspoint/boot.log.bak");
        Storage.rename("/.crosspoint/boot.log", "/.crosspoint/boot.log.bak");
      } else if (check) {
        check.close();
      }
    }
    if ((bootLog = Storage.open("/.crosspoint/boot.log", O_RDWR | O_CREAT | O_AT_END))) {
      char buf[160];
      snprintf(buf, sizeof(buf), "version=%s reset=%s heap=%u uptime=%lu\n", CROSSPOINT_VERSION, resetStr,
               ESP.getFreeHeap(), millis());
      bootLog.print(buf);
      bootLog.close();
      LOG_INF("MAIN", "Boot: version=%s reset=%s", CROSSPOINT_VERSION, resetStr);
    }
  }
  KOREADER_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // For normal wakeups, verify power button press duration
      LOG_DBG("MAIN", "Verifying power button press duration");
      verifyPowerButtonDuration();
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts();

  activityManager.goToBoot();

  // Check for SD card firmware update
  if (Storage.exists("/firmware.bin")) {
    LOG_INF("MAIN", "SD card firmware update found, applying...");
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const int barX = 40;
    const int barW = pageWidth - 80;
    const int barH = 12;
    const int barY = pageHeight / 2 + 35;

    // Version string extracted from firmware.bin (set below before first draw)
    char newVersion[64] = "(unknown)";

    auto drawUpdateScreen = [&](int pct) {
      renderer.clearScreen();
      renderer.drawCenteredText(PULSR_10_FONT_ID, pageHeight / 2 - 20, "Updating firmware...", true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10, "Do not power off");
      renderer.drawRect(barX, barY, barW, barH, true);
      if (pct > 0) {
        const int fillW = (barW * pct) / 100;
        if (fillW > 2) renderer.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, true);
      }
      char pctStr[8];
      snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
      renderer.drawCenteredText(SMALL_FONT_ID, barY + barH + 8, pctStr);
      char curBuf[80];
      char instBuf[80];
      snprintf(curBuf, sizeof(curBuf), "Current: %s", CROSSPOINT_VERSION);
      snprintf(instBuf, sizeof(instBuf), "Installing: %s", newVersion);
      renderer.drawCenteredText(SMALL_FONT_ID, barY + barH + 26, curBuf);
      renderer.drawCenteredText(SMALL_FONT_ID, barY + barH + 44, instBuf);
      renderer.displayBuffer();
    };

    auto logOtaError = [](const char* msg, size_t fileSize = 0, size_t written = 0) -> bool {
      FsFile logFile;
      if (!Storage.openFileForWrite("MAIN", "/.crosspoint/ota_error.log", logFile)) {
        LOG_ERR("MAIN", "OTA: could not open /.crosspoint/ota_error.log for writing");
        return false;
      }
      logFile.print("OTA error: ");
      logFile.println(msg);
      if (fileSize > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "File size: %u, Written: %u", (unsigned)fileSize, (unsigned)written);
        logFile.println(buf);
      }
      logFile.print("Free heap: ");
      logFile.println(ESP.getFreeHeap());
      logFile.print("Version: ");
      logFile.println(CROSSPOINT_VERSION);
      logFile.close();
      return true;
    };

    auto showError = [&](const char* msg, size_t fileSize = 0, size_t written = 0) {
      Storage.remove("/firmware.bin");  // Always delete — prevents re-flash loop on next boot
      const bool logged = logOtaError(msg, fileSize, written);
      renderer.clearScreen();
      renderer.drawCenteredText(PULSR_10_FONT_ID, pageHeight / 2 - 30, "Firmware update failed", true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2, msg);
      renderer.drawCenteredText(
          SMALL_FONT_ID, pageHeight / 2 + 20,
          logged ? "Error saved to /.crosspoint/ota_error.log" : "Could not write /.crosspoint/ota_error.log");
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      delay(15000);
    };

    // Extract version string from firmware.bin by scanning for "CrossPoint-ESP32-" prefix
    // (embedded as part of the User-Agent string literal in OtaUpdater.cpp).
    // Scans up to 1MB in 4KB chunks — avoids loading the whole 6MB binary into RAM.
    {
      constexpr const char prefix[] = "CrossPoint-ESP32-";
      constexpr size_t prefixLen = sizeof(prefix) - 1;
      auto* scanBuf = static_cast<char*>(malloc(4096));
      if (scanBuf) {
        FsFile vf = Storage.open("/firmware.bin");
        if (vf) {
          size_t scanned = 0;
          bool found = false;
          while (!found && scanned < 1024u * 1024u) {
            const size_t got = vf.read(scanBuf, 4096);
            if (got == 0) break;
            for (size_t i = 0; i + prefixLen < got && !found; i++) {
              if (memcmp(scanBuf + i, prefix, prefixLen) == 0) {
                const char* ver = scanBuf + i + prefixLen;
                size_t vLen = 0;
                while (vLen < 63 && i + prefixLen + vLen < got && static_cast<uint8_t>(ver[vLen]) >= 0x20 &&
                       static_cast<uint8_t>(ver[vLen]) <= 0x7e) {
                  vLen++;
                }
                if (vLen > 0) {
                  memcpy(newVersion, ver, vLen);
                  newVersion[vLen] = '\0';
                  found = true;
                }
              }
            }
            scanned += got;
          }
          vf.close();
        }
        free(scanBuf);
      }
      LOG_INF("MAIN", "New firmware version: %s", newVersion);
    }

    // Skip install if firmware.bin is the same version already running
    if (strcmp(newVersion, CROSSPOINT_VERSION) == 0) {
      LOG_INF("MAIN", "firmware.bin is same version (%s) — skipping install, deleting file", CROSSPOINT_VERSION);
      Storage.remove("/firmware.bin");
      renderer.clearScreen();
      renderer.drawCenteredText(PULSR_10_FONT_ID, renderer.getScreenHeight() / 2 - 20, "Firmware already up to date",
                                true, EpdFontFamily::BOLD);
      char verBuf[80];
      snprintf(verBuf, sizeof(verBuf), "Running: %s", CROSSPOINT_VERSION);
      renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 10, verBuf);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      delay(3000);
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);  // wipe message before activity manager starts
      return;
    }

    drawUpdateScreen(0);

    FsFile firmwareFile = Storage.open("/firmware.bin");
    if (firmwareFile) {
      const size_t fileSize = firmwareFile.size();
      LOG_INF("MAIN", "Firmware file size: %u bytes", fileSize);

      if (fileSize == 0) {
        firmwareFile.close();
        LOG_ERR("MAIN", "firmware.bin is empty (0 bytes) — aborting update");
        showError("firmware.bin is empty (0 bytes)");
      } else {
        // Use ESP-IDF OTA API: always write to the INACTIVE partition so the
        // running partition is never touched (safe even if flash is interrupted).
        const esp_partition_t* runningPart = esp_ota_get_running_partition();
        const esp_partition_t* updatePart = esp_ota_get_next_update_partition(nullptr);
        LOG_INF("MAIN", "OTA: running=%s@0x%lx  target=%s@0x%lx", runningPart ? runningPart->label : "?",
                runningPart ? (unsigned long)runningPart->address : 0UL, updatePart ? updatePart->label : "?",
                updatePart ? (unsigned long)updatePart->address : 0UL);

        if (!updatePart) {
          firmwareFile.close();
          LOG_ERR("MAIN", "No OTA update partition found");
          showError("no update partition found");
        } else {
          esp_ota_handle_t otaHandle = 0;
          esp_err_t err = esp_ota_begin(updatePart, OTA_SIZE_UNKNOWN, &otaHandle);
          if (err != ESP_OK) {
            firmwareFile.close();
            char errMsg[80];
            snprintf(errMsg, sizeof(errMsg), "ota_begin: %s", esp_err_to_name(err));
            LOG_ERR("MAIN", "OTA begin failed: %s", errMsg);
            showError(errMsg, fileSize);
          } else {
            drawUpdateScreen(0);
            size_t written = 0;
            int lastPct = 0;
            bool writeOk = true;
            uint8_t buf[4096];
            while (written < fileSize) {
              const size_t toRead = min(sizeof(buf), fileSize - written);
              const int bytesRead = firmwareFile.read(buf, toRead);
              if (bytesRead <= 0) {
                writeOk = false;
                break;
              }
              err = esp_ota_write(otaHandle, buf, static_cast<size_t>(bytesRead));
              if (err != ESP_OK) {
                writeOk = false;
                break;
              }
              written += static_cast<size_t>(bytesRead);
              yield();  // Feed watchdog during long SD read
              const int pct = static_cast<int>((written * 100) / fileSize);
              if (pct >= lastPct + 5) {
                lastPct = pct;
                drawUpdateScreen(pct);
              }
            }
            firmwareFile.close();

            if (!writeOk || written != fileSize) {
              esp_ota_abort(otaHandle);
              char errMsg[80];
              if (!writeOk) {
                snprintf(errMsg, sizeof(errMsg), "ota_write at %u/%u: %s", (unsigned)written, (unsigned)fileSize,
                         esp_err_to_name(err));
              } else {
                snprintf(errMsg, sizeof(errMsg), "short read %u/%u", (unsigned)written, (unsigned)fileSize);
              }
              LOG_ERR("MAIN", "OTA write failed: %s", errMsg);
              showError(errMsg, fileSize, written);
            } else {
              err = esp_ota_end(otaHandle);
              // ESP_ERR_OTA_VALIDATE_FAILED means SHA256 didn't match — expected for
              // Arduino/unsigned builds that don't embed a hash. Data was written
              // correctly; proceed to set_boot_partition anyway.
              if (err != ESP_OK && err != ESP_ERR_OTA_VALIDATE_FAILED) {
                char errMsg[80];
                snprintf(errMsg, sizeof(errMsg), "ota_end: %s", esp_err_to_name(err));
                LOG_ERR("MAIN", "OTA end/validate failed: %s", errMsg);
                showError(errMsg, fileSize, written);
              } else {
                if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                  LOG_INF("MAIN", "OTA SHA256 validation skipped (unsigned build)");
                }
                // Use forceSetBootPartition — bypasses esp_ota_set_boot_partition()'s
                // image validation, which returns ESP_ERR_OTA_VALIDATE_FAILED for unsigned
                // Arduino builds (no embedded SHA256). forceSetBootPartition writes the
                // otadata entry directly with state=VALID and correct CRC.
                // forceSetBootPartition(self) in setup() re-confirms on next boot.
                err = forceSetBootPartition(updatePart);
                if (err != ESP_OK) {
                  char errMsg[80];
                  snprintf(errMsg, sizeof(errMsg), "set_boot: %s", esp_err_to_name(err));
                  LOG_ERR("MAIN", "OTA set_boot_partition failed: %s", errMsg);
                  showError(errMsg);
                } else {
                  if (!Storage.remove("/firmware.bin")) {
                    LOG_INF("MAIN", "OTA done but could not delete firmware.bin");
                  }
                  LOG_INF("MAIN", "Firmware update complete, restarting...");
                  ESP.restart();
                }
              }
            }
          }
        }
      }
    } else {
      LOG_ERR("MAIN", "Failed to open /firmware.bin");
      showError("Failed to open /firmware.bin");
    }
  }

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
  // crashed (indicated by readerActivityLoadCount > 0)
  if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    teardownDangerZone("opening epub on boot");
    activityManager.goToReader(path);
  }

  // Danger Zone: auto-connect WiFi + start web server + feed sync
  dangerZoneAutoConnect();
  if (dzWifiConnected) {
    // Hand off to the web server activity in pre-connected mode.
    // Stop the DZ background server first — the activity will start its own.
    if (dzWebServer) {
      dzWebServer->stop();
      dzWebServer.reset();
    }
    activityManager.replaceActivity(
        std::make_unique<CrossPointWebServerActivity>(renderer, mappedInputManager, /*preConnected=*/true));
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void runScreenshotTour() {
  auto captureStep = [](const char* name) {
    // Give the activity time to render
    for (int i = 0; i < 6; i++) {
      activityManager.loop();
      delay(100);
    }
    ScreenCapture::save(renderer, name);
  };

  GUI.drawPopup(renderer, "Taking screenshots...");
  delay(300);

  activityManager.goToBoot();
  captureStep("home");

  activityManager.goToSettings();
  captureStep("settings");

  activityManager.goToFileBrowser();
  captureStep("browse");

  activityManager.goToRecentBooks();
  captureStep("recents");

  if (!APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
    captureStep("reader");
  }

  activityManager.goToBrowser();
  captureStep("opds");

  activityManager.goToFileTransfer();
  captureStep("file_transfer");

  activityManager.replaceActivity(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInputManager));
  captureStep("network_mode");

  activityManager.replaceActivity(std::make_unique<WifiSelectionActivity>(renderer, mappedInputManager, false));
  captureStep("wifi_scan");

  activityManager.replaceActivity(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInputManager, "WIFI PASSWORD", "", 64, true));
  captureStep("keyboard");

  activityManager.goToBoot();
  delay(300);
  GUI.drawPopup(renderer, "Done! Saved to /screencap/");
  delay(2000);
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        logSerial.printf("SCREENSHOT_START:%d\n", HalDisplay::BUFFER_SIZE);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, HalDisplay::BUFFER_SIZE);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || activityManager.preventAutoSleep() || gpio.isUsbConnected()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Redraw when USB connection state changes (CHRG pill on/off)
  {
    static bool lastUsbState = gpio.isUsbConnected();
    const bool usbNow = gpio.isUsbConnected();
    if (usbNow != lastUsbState) {
      lastUsbState = usbNow;
      activityManager.requestUpdate();
    }
  }

  static bool screenshotButtonsReleased = true;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  } else {
    screenshotButtonsReleased = true;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  // Suppress auto-sleep when USB is connected, or when Danger Zone is active (web server must stay reachable)
  const bool suppressSleep = gpio.isUsbConnected() || SETTINGS.dangerZoneEnabled;
  if (millis() - lastActivityTime >= sleepTimeoutMs && !suppressSleep) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    teardownDangerZone("inactivity-timeout");
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Screenshot tour combo: Power + Confirm held 1.5 s
  {
    static unsigned long screenshotHoldStart = 0;
    static bool screenshotTriggered = false;
    const bool powerHeld = gpio.isPressed(HalGPIO::BTN_POWER);
    const bool confirmHeld = gpio.isPressed(HalGPIO::BTN_CONFIRM);
    if (powerHeld && confirmHeld && !screenshotTriggered) {
      if (screenshotHoldStart == 0)
        screenshotHoldStart = millis();
      else if (millis() - screenshotHoldStart >= 1500) {
        screenshotTriggered = true;
        runScreenshotTour();
        // Restore WiFi after tour (button-combo path disconnects for clean screenshots)
        reconnectWifiAfterTour();
        if (SETTINGS.dangerZoneEnabled) dangerZoneAutoConnect();
      }
    } else {
      if (!powerHeld || !confirmHeld) screenshotHoldStart = 0;
      if (!powerHeld) screenshotTriggered = false;
    }
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If a screenshot combination is being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN) || gpio.isPressed(HalGPIO::BTN_CONFIRM)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // Danger Zone: service background web server when running outside CrossPointWebServerActivity
  if (dzWebServer && dzWebServer->isRunning()) {
    dzWebServer->handleClient();
  }

  // Danger Zone: handle screenshot tour request from API
  if (dzScreenshotTourRequested) {
    dzScreenshotTourRequested = false;

    // Stop DZ web server and WiFi before the tour (tour changes activities)
    if (dzWebServer) {
      dzWebServer->stop();
      dzWebServer.reset();
      UITheme::setHttpServerActive(false);
    }
    UITheme::setNetworkStatus(false, false);
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    dzWifiConnected = false;

    // Run the screenshot tour (skipping WiFi/network activities)
    runScreenshotTour();

    // Restore WiFi connectivity (always), then restart DZ web server if enabled
    reconnectWifiAfterTour();
    dangerZoneAutoConnect();

    // Return to home screen
    activityManager.goHome();
  }

  // Danger Zone: handle flash firmware request from API.
  // Rather than doing an in-process OTA (which is unreliable while WiFi is active),
  // we leave firmware.bin on the SD card and reboot.  The boot-time OTA path in
  // setup() detects /firmware.bin and flashes it reliably from a clean state.
  if (dzFlashRequested) {
    dzFlashRequested = false;

    if (!Storage.exists("/firmware.bin")) {
      LOG_ERR("DZ", "Flash requested but /firmware.bin not found on SD");
    } else {
      LOG_INF("DZ", "firmware.bin ready — rebooting to install via boot-time OTA...");
      ESP.restart();
    }
  }

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}