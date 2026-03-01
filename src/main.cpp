#include <Arduino.h>
#include <Epub.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <Update.h>
#include <esp_ota_ops.h>

// Run before global constructors to prevent OTA rollback even if a global crashes
__attribute__((constructor(101))) static void earlyMarkOtaValid() {
  esp_ota_mark_app_valid_cancel_rollback();
}

#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/ScreenCapture.h"
#include "WifiCredentialStore.h"
#include "network/CrossPointWebServer.h"
#include "network/RssFeedSync.h"
#include <WiFi.h>
#include <ESPmDNS.h>

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

// Enter deep sleep mode
void enterDeepSleep() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation

  // Shut down Danger Zone background web server if running
  if (dzWebServer) {
    dzWebServer->stop();
    dzWebServer.reset();
    dzWifiConnected = false;
    UITheme::setHttpServerActive(false);
    UITheme::setNetworkStatus(false, false);
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }

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
  renderer.insertFont(PULSR_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(PULSR_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(PULSR_10_FONT_ID, pulsr10FontFamily);
  renderer.insertFont(PULSR_12_FONT_ID, pulsr12FontFamily);
  LOG_DBG("MAIN", "Fonts setup");
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

  // Block up to 15 seconds for connection
  constexpr unsigned long TIMEOUT_MS = 15000;
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("DZ", "Auto-connect failed (status=%d)", WiFi.status());
    WiFi.mode(WIFI_OFF);
    return;
  }

  dzWifiConnected = true;
  LOG_INF("DZ", "Connected! IP=%s", WiFi.localIP().toString().c_str());

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

  // Start RSS feed sync
  RssFeedSync::startSync();
}

void setup() {
  t1 = millis();

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

  SETTINGS.loadFromFile();
  // DEBUG: force Danger Zone on so OpenClaw can troubleshoot remotely
  SETTINGS.dangerZoneEnabled = true;
  strncpy(SETTINGS.dangerZonePassword, "1234", sizeof(SETTINGS.dangerZonePassword) - 1);
  I18N.loadSettings();

  // Boot log: write early so any subsequent crash is detectable
  {
    const esp_reset_reason_t resetReason = esp_reset_reason();
    const char* resetStr = (resetReason == ESP_RST_PANIC)    ? "panic"    :
                           (resetReason == ESP_RST_INT_WDT)  ? "int_wdt"  :
                           (resetReason == ESP_RST_TASK_WDT) ? "task_wdt" :
                           (resetReason == ESP_RST_WDT)      ? "wdt"      :
                           (resetReason == ESP_RST_BROWNOUT) ? "brownout" :
                           (resetReason == ESP_RST_SW)       ? "sw"       :
                           (resetReason == ESP_RST_POWERON)  ? "poweron"  :
                           (resetReason == ESP_RST_DEEPSLEEP)? "deepsleep": "other";
    FsFile bootLog;
    // Rotate log if over 2KB
    {
      FsFile check = Storage.open("/boot.log");
      if (check && check.size() > 2048) {
        check.close();
        Storage.remove("/boot.log.bak");
        Storage.rename("/boot.log", "/boot.log.bak");
      } else if (check) {
        check.close();
      }
    }
    if ((bootLog = Storage.open("/boot.log", O_RDWR | O_CREAT | O_AT_END))) {
      char buf[160];
      snprintf(buf, sizeof(buf), "version=%s reset=%s heap=%u uptime=%lu\n",
               CROSSPOINT_VERSION, resetStr, ESP.getFreeHeap(), millis());
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

    auto drawUpdateScreen = [&](int pct) {
      renderer.clearScreen();
      renderer.drawCenteredText(PULSR_10_FONT_ID, pageHeight / 2 - 20, "Updating firmware...", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10, "Do not power off");
      renderer.drawRect(barX, barY, barW, barH, true);
      if (pct > 0) {
        const int fillW = (barW * pct) / 100;
        if (fillW > 2) renderer.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, true);
      }
      char pctStr[8];
      snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
      renderer.drawCenteredText(SMALL_FONT_ID, barY + barH + 8, pctStr);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
      renderer.displayBuffer();
    };

    auto logOtaError = [](const char* msg, size_t fileSize = 0, size_t written = 0) -> bool {
      FsFile logFile;
      if (!Storage.openFileForWrite("MAIN", "/ota_error.log", logFile)) {
        LOG_ERR("MAIN", "OTA: could not open /ota_error.log for writing");
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
      const bool logged = logOtaError(msg, fileSize, written);
      renderer.clearScreen();
      renderer.drawCenteredText(PULSR_10_FONT_ID, pageHeight / 2 - 30, "Firmware update failed", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2, msg);
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, "firmware.bin NOT deleted");
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 38,
                                logged ? "Error saved to /ota_error.log" : "Could not write /ota_error.log");
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      delay(15000);
    };

    drawUpdateScreen(0);

    FsFile firmwareFile = Storage.open("/firmware.bin");
    if (firmwareFile) {
      const size_t fileSize = firmwareFile.size();
      LOG_INF("MAIN", "Firmware file size: %u bytes", fileSize);

      if (fileSize == 0) {
        firmwareFile.close();
        LOG_ERR("MAIN", "firmware.bin is empty (0 bytes) — aborting update");
        showError("firmware.bin is empty (0 bytes)");
      } else if (Update.begin(fileSize, U_FLASH)) {
        int lastPct = 0;
        Update.onProgress([&](size_t progress, size_t total) {
          if (total == 0) return;
          const int pct = std::min(100, static_cast<int>((progress * 100) / total));
          if (pct >= lastPct + 5) {
            lastPct = pct;
            drawUpdateScreen(pct);
          }
        });
        // HalFile is Print-only (not Stream), so read+write in chunks
        size_t written = 0;
        uint8_t buf[4096];
        while (written < fileSize) {
          const size_t toRead = min(sizeof(buf), fileSize - written);
          const int bytesRead = firmwareFile.read(buf, toRead);
          if (bytesRead <= 0) break;
          const size_t bytesWritten = Update.write(buf, bytesRead);
          if (bytesWritten != static_cast<size_t>(bytesRead)) break;
          written += bytesWritten;
          yield();  // Feed watchdog during long SD read
        }
        firmwareFile.close();
        if (written == fileSize && Update.end()) {
          if (!Storage.remove("/firmware.bin")) {
            LOG_INF("MAIN", "Firmware installed but failed to delete firmware.bin from SD card");
          }
          LOG_INF("MAIN", "Firmware update complete, restarting...");
          ESP.restart();
        } else {
          Update.abort();
          char errMsg[80];
          const char* updateErr = Update.errorString();
          if (written != fileSize) {
            snprintf(errMsg, sizeof(errMsg), "short write %u/%u: %s", (unsigned)written, (unsigned)fileSize, updateErr);
          } else {
            snprintf(errMsg, sizeof(errMsg), "finalize failed: %s", updateErr);
          }
          LOG_ERR("MAIN", "Firmware update failed: %s", errMsg);
          showError(errMsg, fileSize, written);
        }
      } else {
        firmwareFile.close();
        char errMsg[80];
        snprintf(errMsg, sizeof(errMsg), "begin failed: %s", Update.errorString());
        LOG_ERR("MAIN", "Update.begin() failed: %s", errMsg);
        showError(errMsg, fileSize);
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
    activityManager.goToReader(path);
  }

  // Danger Zone: auto-connect WiFi + start web server + feed sync
  dangerZoneAutoConnect();

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

  activityManager.goToMyLibrary();
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

  activityManager.replaceActivity(std::make_unique<KeyboardEntryActivity>(renderer, mappedInputManager, "WIFI PASSWORD", "", 64, true));
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
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
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
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Screenshot tour combo: Power + Confirm held 1.5 s
  {
    static unsigned long screenshotHoldStart = 0;
    static bool screenshotTriggered = false;
    const bool powerHeld   = gpio.isPressed(HalGPIO::BTN_POWER);
    const bool confirmHeld = gpio.isPressed(HalGPIO::BTN_CONFIRM);
    if (powerHeld && confirmHeld && !screenshotTriggered) {
      if (screenshotHoldStart == 0) screenshotHoldStart = millis();
      else if (millis() - screenshotHoldStart >= 1500) {
        screenshotTriggered = true;
        runScreenshotTour();
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

    // Auto-reconnect WiFi and restart web server
    dangerZoneAutoConnect();

    // Return to home screen
    activityManager.goHome();
  }

  // Danger Zone: handle flash firmware request from API
  if (dzFlashRequested) {
    dzFlashRequested = false;

    if (!Storage.exists("/firmware.bin")) {
      LOG_ERR("DZ", "Flash requested but /firmware.bin not found");
    } else {
      LOG_INF("DZ", "Flashing firmware from /firmware.bin via API...");

      // Stop DZ web server before flashing (OTA disables flash cache)
      if (dzWebServer) {
        dzWebServer->stop();
        dzWebServer.reset();
        UITheme::setHttpServerActive(false);
      }
      UITheme::setNetworkStatus(false, false);
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
      dzWifiConnected = false;

      FsFile firmwareFile = Storage.open("/firmware.bin");
      if (firmwareFile) {
        const size_t fileSize = firmwareFile.size();
        if (fileSize > 0 && Update.begin(fileSize, U_FLASH)) {
          size_t written = 0;
          uint8_t buf[4096];
          while (written < fileSize) {
            const size_t toRead = min(sizeof(buf), fileSize - written);
            const int bytesRead = firmwareFile.read(buf, toRead);
            if (bytesRead <= 0) break;
            const size_t bytesWritten = Update.write(buf, bytesRead);
            if (bytesWritten != static_cast<size_t>(bytesRead)) break;
            written += bytesWritten;
            yield();  // Feed watchdog during long SD read
          }
          firmwareFile.close();
          if (written == fileSize && Update.end()) {
            Storage.remove("/firmware.bin");
            LOG_INF("DZ", "Firmware flash complete, restarting...");
            ESP.restart();
          } else {
            Update.abort();
            const char* updateErr = Update.errorString();
            char errMsg[120];
            if (written != fileSize) {
              snprintf(errMsg, sizeof(errMsg), "short write %u/%u: %s", (unsigned)written, (unsigned)fileSize, updateErr);
            } else {
              snprintf(errMsg, sizeof(errMsg), "end() failed: %s (wrote %u)", updateErr, (unsigned)written);
            }
            LOG_ERR("DZ", "Firmware flash failed: %s", errMsg);
            // Write error to SD log for later retrieval
            FsFile logFile;
            if (Storage.openFileForWrite("DZ", "/ota_error.log", logFile)) {
              logFile.print(errMsg);
              logFile.close();
            }
          }
        } else {
          firmwareFile.close();
          char errMsg[80];
          snprintf(errMsg, sizeof(errMsg), "begin failed: %s", Update.errorString());
          LOG_ERR("DZ", "Update.begin() failed: %s", errMsg);
        }
      }
      // If flash failed, reconnect WiFi so the device is still reachable
      dangerZoneAutoConnect();
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