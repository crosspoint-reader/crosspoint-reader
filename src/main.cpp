#include <Arduino.h>
#include <EpdFont.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "UsbSerialProtocol.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "core/CoreBootstrap.h"
#include "core/features/FeatureLifecycle.h"
#include "core/features/FeatureModules.h"
#include "fontIds.h"
#include "network/BackgroundWebServer.h"
#include "network/BackgroundWifiService.h"
#include "util/ButtonNavigator.h"
#include "util/FactoryResetUtils.h"
#include "util/FirmwareUpdateUtil.h"
#include "util/ScreenshotUtil.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
BackgroundWebServer& backgroundServer = BackgroundWebServer::getInstance();

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#if ENABLE_BOOKERLY_FONTS
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
#endif  // ENABLE_BOOKERLY_FONTS

#if ENABLE_NOTOSANS_FONTS
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
#endif  // ENABLE_NOTOSANS_FONTS

#if ENABLE_OPENDYSLEXIC_FONTS
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
#endif  // ENABLE_OPENDYSLEXIC_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

unsigned long t1 = 0;
unsigned long t2 = 0;

namespace {
constexpr char kCrossPointDataDir[] = "/.crosspoint";
constexpr char kFactoryResetMarkerFile[] = "/.factory-reset-pending";
constexpr char kUsbMscSessionMarkerFile[] = "/.crosspoint/usb-msc-active";

enum class UsbMscSessionState { Idle, Prompt, Active };

UsbMscSessionState usbMscSessionState = UsbMscSessionState::Idle;
bool usbConnectedLast = false;
UsbSerialProtocol usbSerialProtocol;
bool usbMscScreenNeedsRedraw = false;
bool usbMscRemountPending = false;

void renderUsbMscPrompt() {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 260, "Connect as Mass Storage?", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 300, "SD card will be unavailable on-device", true);
  const auto labels = mappedInputManager.mapLabels(tr(STR_NO), tr(STR_YES), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void renderUsbMscLockedScreen() {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 260, "Mass Storage Active", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 300, "Disconnect USB cable to return", true);
  renderer.displayBuffer();
}

void enterUsbMscSession() {
  LOG_INF("USBMSC", "Entering USB mass storage lock mode");
  APP_STATE.saveToFile();
  if (!SETTINGS.saveToFile()) {
    LOG_WRN("USBMSC", "Failed to persist settings before USB MSC session");
  }

  activityManager.goHome();
  activityManager.loop();

  Storage.mkdir(kCrossPointDataDir);
  Storage.writeFile(kUsbMscSessionMarkerFile, "1");
  usbSerialProtocol.reset();
  usbMscSessionState = UsbMscSessionState::Active;
  usbMscScreenNeedsRedraw = true;
}

void exitUsbMscSession() {
  LOG_INF("USBMSC", "Exiting USB mass storage lock mode");
  usbMscSessionState = UsbMscSessionState::Idle;
  usbMscScreenNeedsRedraw = false;
  usbMscRemountPending = true;
}

void applyPendingFactoryReset() {
  if (!Storage.exists(kFactoryResetMarkerFile)) {
    return;
  }

  LOG_INF("RESET", "Pending factory reset marker detected");

  if (!FactoryResetUtils::resetCrossPointMetadataPreservingContent()) {
    LOG_ERR("RESET", "Failed to reset CrossPoint metadata/cache. Retrying on next boot.");
    return;
  }

  if (!Storage.remove(kFactoryResetMarkerFile)) {
    LOG_ERR("RESET", "Metadata reset completed, but marker removal failed: %s", kFactoryResetMarkerFile);
    return;
  }

  LOG_INF("RESET", "Factory reset completed from pending marker (cache cleared, user files preserved)");
}
}  // namespace

// True if BG_WIFI.start() was called this wake — used by enterDeepSleep() to
// decide whether to update the WiFi auto-connect backoff counters.
static bool wifiAutoConnectAttempted = false;

void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    return;
  }

  const auto start = millis();
  bool abort = false;
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);
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

void enterDeepSleep() {
  HalPowerManager::Lock powerLock;
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Update WiFi auto-connect backoff before sleeping (only if we attempted this wake)
  if (wifiAutoConnectAttempted) {
    const bool hadActivity = BG_WIFI.hadApiActivity();
    BG_WIFI.stop();
    if (hadActivity) {
      // Successful sync: reset backoff — try again next wake
      APP_STATE.wifiAutoConnectBackoffLevel = 0;
      APP_STATE.wifiAutoConnectSkipCount = 0;
      LOG_DBG("MAIN", "WiFi had API activity — backoff reset");
    } else {
      // No push/pull received: increase backoff exponentially (cap at level 4 = 15 skips)
      if (APP_STATE.wifiAutoConnectBackoffLevel < 4) {
        APP_STATE.wifiAutoConnectBackoffLevel++;
      }
      APP_STATE.wifiAutoConnectSkipCount = (1U << APP_STATE.wifiAutoConnectBackoffLevel) - 1U;
      LOG_DBG("MAIN", "WiFi no API activity — backoff level %d, next skip: %d", APP_STATE.wifiAutoConnectBackoffLevel,
              APP_STATE.wifiAutoConnectSkipCount);
    }
  }

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

  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  renderer.setFontDecompressor(&fontDecompressor);
  renderer.insertFontFamily(BOOKERLY_14_FONT_ID, &bookerly14FontFamily);
#if ENABLE_BOOKERLY_FONTS
  renderer.insertFontFamily(BOOKERLY_12_FONT_ID, &bookerly12FontFamily);
  renderer.insertFontFamily(BOOKERLY_16_FONT_ID, &bookerly16FontFamily);
  renderer.insertFontFamily(BOOKERLY_18_FONT_ID, &bookerly18FontFamily);
#endif  // ENABLE_BOOKERLY_FONTS
#if ENABLE_NOTOSANS_FONTS
  renderer.insertFontFamily(NOTOSANS_12_FONT_ID, &notosans12FontFamily);
  renderer.insertFontFamily(NOTOSANS_14_FONT_ID, &notosans14FontFamily);
  renderer.insertFontFamily(NOTOSANS_16_FONT_ID, &notosans16FontFamily);
  renderer.insertFontFamily(NOTOSANS_18_FONT_ID, &notosans18FontFamily);
#endif  // ENABLE_NOTOSANS_FONTS
#if ENABLE_OPENDYSLEXIC_FONTS
  renderer.insertFontFamily(OPENDYSLEXIC_8_FONT_ID, &opendyslexic8FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_10_FONT_ID, &opendyslexic10FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_12_FONT_ID, &opendyslexic12FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_14_FONT_ID, &opendyslexic14FontFamily);
#endif  // ENABLE_OPENDYSLEXIC_FONTS
  renderer.insertFontFamily(UI_10_FONT_ID, &ui10FontFamily);
  renderer.insertFontFamily(UI_12_FONT_ID, &ui12FontFamily);
  renderer.insertFontFamily(SMALL_FONT_ID, &smallFontFamily);

  core::FeatureLifecycle::onFontSetup(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

  gpio.begin();
  powerManager.begin();

  const bool usbConnectedAtBoot = gpio.isUsbConnected();
  if (usbConnectedAtBoot) {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) {
      delay(10);
    }
  }
  core::CoreBootstrap::initializeFeatureSystem(usbConnectedAtBoot);

  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  core::FeatureLifecycle::onStorageReady();

  applyPendingFactoryReset();
  if (core::FeatureModules::hasCapability(core::Capability::UsbMassStorage)) {
    usbConnectedLast = gpio.isUsbConnected();
    if (Storage.exists(kUsbMscSessionMarkerFile)) {
      LOG_WRN("USBMSC", "Detected stale USB MSC marker; recovering SD ownership");
      Storage.remove(kUsbMscSessionMarkerFile);
    }
  }

  SETTINGS.loadFromFile();
  core::FeatureLifecycle::onSettingsLoaded(renderer);
  I18N.loadSettings();
  WIFI_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const bool wokeFromSleep = (gpio.getWakeupReason() == HalGPIO::WakeupReason::PowerButton);

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      verifyPowerButtonDuration();
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // USB power connected: stay awake so user can access the file server
      LOG_DBG("MAIN", "Wakeup reason: After USB Power - staying awake for file transfer");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts();

  if (FirmwareUpdateUtil::checkForLocalUpdate()) {
    FirmwareUpdateUtil::performLocalUpdate(renderer);
  }

  activityManager.goToBoot();

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    activityManager.goHome();
  } else {
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath.clear();
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  // WiFi auto-connect on wake from sleep (background, silent)
  if (wokeFromSleep && SETTINGS.wifiAutoConnect) {
    if (APP_STATE.wifiAutoConnectSkipCount > 0) {
      // Still in backoff — consume one skip cycle
      APP_STATE.wifiAutoConnectSkipCount--;
      APP_STATE.saveToFile();
      LOG_DBG("MAIN", "WiFi auto-connect skipped (backoff remaining: %d)", APP_STATE.wifiAutoConnectSkipCount);
    } else {
      // Attempt silent background connect using last known credentials
      const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
      if (!lastSsid.empty()) {
        const auto* cred = WIFI_STORE.findCredential(lastSsid);
        if (cred) {
          LOG_DBG("MAIN", "Starting background WiFi auto-connect to: %s", lastSsid.c_str());
          BG_WIFI.start(cred->ssid.c_str(), cred->password.c_str());
          wifiAutoConnectAttempted = true;
        }
      }
    }
  }

  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;
  static unsigned long lastActivityTime = millis();
  static bool screenshotButtonsReleased = true;

  gpio.update();
  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

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

  if (core::FeatureModules::hasCapability(core::Capability::UsbMassStorage)) {
    const bool usbConnected = gpio.isUsbConnected();

    if (usbMscRemountPending) {
      usbMscRemountPending = false;
      Storage.remove(kUsbMscSessionMarkerFile);
      if (!Storage.begin()) {
        LOG_ERR("USBMSC", "SD remount failed after USB MSC exit");
        activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
        activityManager.loop();
        usbConnectedLast = usbConnected;
        return;
      }
      activityManager.goHome();
      usbConnectedLast = usbConnected;
      return;
    }

    if (SETTINGS.usbMscPromptOnConnect && usbConnected && !usbConnectedLast &&
        usbMscSessionState == UsbMscSessionState::Idle) {
      usbMscSessionState = UsbMscSessionState::Prompt;
      usbMscScreenNeedsRedraw = true;
    }

    if (usbMscSessionState == UsbMscSessionState::Prompt) {
      backgroundServer.loop(usbConnected, false);
      if (!usbConnected) {
        usbMscSessionState = UsbMscSessionState::Idle;
        activityManager.requestUpdate(true);
      } else {
        if (usbMscScreenNeedsRedraw) {
          renderUsbMscPrompt();
          usbMscScreenNeedsRedraw = false;
        }
        if (mappedInputManager.wasReleased(MappedInputManager::Button::Confirm)) {
          enterUsbMscSession();
        } else if (mappedInputManager.wasReleased(MappedInputManager::Button::Back)) {
          usbMscSessionState = UsbMscSessionState::Idle;
          activityManager.requestUpdate(true);
        }
      }
      usbConnectedLast = usbConnected;
      delay(20);
      return;
    }

    if (usbMscSessionState == UsbMscSessionState::Active) {
      usbSerialProtocol.loop();
      if (!usbConnected) {
        exitUsbMscSession();
      } else if (usbMscScreenNeedsRedraw) {
        renderUsbMscLockedScreen();
        usbMscScreenNeedsRedraw = false;
      }
      usbConnectedLast = usbConnected;
      delay(20);
      return;
    }

    usbConnectedLast = usbConnected;
  }

  {
    const bool usbConn = gpio.isUsbConnected();
    const bool allowRun = core::FeatureModules::hasCapability(core::Capability::BackgroundServer) &&
                          SETTINGS.backgroundServerOnCharge && usbConn && !activityManager.blocksBackgroundServer();
    backgroundServer.loop(usbConn, allowRun);
  }

  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || activityManager.preventAutoSleep() ||
      backgroundServer.shouldPreventAutoSleep()) {
    lastActivityTime = millis();
    powerManager.setPowerSaving(false);
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
    }
    return;
  }
  screenshotButtonsReleased = true;

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    return;
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  if (activityManager.skipLoopDelay() || backgroundServer.wantsFastLoop()) {
    powerManager.setPowerSaving(false);
    yield();
  } else if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
    powerManager.setPowerSaving(true);
    delay(50);
  } else {
    delay(10);
  }
}
