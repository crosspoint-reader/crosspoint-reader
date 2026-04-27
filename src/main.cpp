#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>
#include <esp_ota_ops.h>

#include <cstring>
#include <vector>

#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "WeatherSettingsStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio);
ButtonEventManager buttonEventManager(mappedInputManager);
ButtonEventManager& globalButtonEvents() { return buttonEventManager; }
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap());

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

// Enter deep sleep mode
void enterDeepSleep() {
  LOG_DBG("MAIN", "enterDeepSleep called at millis=%lu, powerBtn isPressed=%d, rawPin=%d", millis(),
          gpio.isPressed(HalGPIO::BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  // On X3 the DS3231 keeps time independently, so there's no need to keep the MCU
  // powered during deep sleep for LP timer preservation.
  const bool keepLpAlive = SETTINGS.useClock && !gpio.deviceIsX3();
  HalClock::saveBeforeSleep(keepLpAlive);
  APP_STATE.saveToFile();

  activityManager.goToSleep();

  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep (powerBtn isPressed=%d, rawPin=%d)", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  powerManager.startDeepSleep(gpio, keepLpAlive);
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
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
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
  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  {
    esp_ota_img_states_t otaState;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }
  HalSystem::begin();
  gpio.begin();
  powerManager.begin();
  gpio_deep_sleep_hold_dis();  // Release deep sleep GPIO hold state from previous sleep cycle

#ifdef ENABLE_SERIAL_LOG
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();
  SETTINGS.loadFromFile();
  HalSystem::clearPanic();  // TODO: move this to an activity when we have one to display the panic info
  HalClock::applyTimezone(SETTINGS.timeZone);
  I18N.loadSettings();
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  WEATHER_SETTINGS.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  LOG_DBG("MAIN", "Wakeup reason: %d, millis=%lu, rawPowerPin=%d", static_cast<int>(wakeupReason), millis(),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton: {
      constexpr uint16_t defaultPowerButtonDurationMs = 400;
      LOG_DBG("MAIN", "Verifying power button press duration (required=%u ms, default only)",
              defaultPowerButtonDurationMs);
      gpio.verifyPowerButtonWakeup(defaultPowerButtonDurationMs, false);
      LOG_DBG("MAIN", "Power button verification passed, millis=%lu", millis());
      break;
    }
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

  APP_STATE.loadFromFile();
  HalClock::restore();
  RECENT_BOOKS.loadFromFile();
  GLOBAL_BOOKMARKS.load();

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
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  buttonEventManager.update();
  HalClock::updatePeriodic();

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setTextDarkness(SETTINGS.textDarkness);

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
        const uint16_t width = display.getDisplayWidth();
        const uint16_t height = display.getDisplayHeight();
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d:%d:%d\n", width, height, bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
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

  // Track power button hold for sleep.  We require a fresh press edge (wasPressed)
  // before starting to measure hold time, so that a hold carried over from boot
  // (wake-up press) is never misinterpreted as a "go to sleep" press.
  // The power button long-press is not user-remappable, so this path always owns it.
  // Sleep mapped to other buttons is handled by the dispatcher's BTN_SLEEP case below.
  static unsigned long powerHoldStart = 0;
  if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = millis();
    LOG_DBG("MAIN", "loop: power button press detected (fresh edge)");
  }
  if (gpio.isPressed(HalGPIO::BTN_POWER) && powerHoldStart > 0) {
    const unsigned long heldTime = millis() - powerHoldStart;
    if (heldTime > SETTINGS.getPowerButtonDuration()) {
      // If the screenshot combination is potentially being pressed, don't sleep
      if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
        return;
      }
      LOG_DBG("MAIN", "loop: power button held for %lu ms (> %u ms), entering deep sleep", heldTime,
              SETTINGS.getPowerButtonDuration());
      enterDeepSleep();
      // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
      return;
    }
  }

  if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = 0;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  // Dispatch globally-configured button actions before handing control to the activity.
  // Only non-Default actions are intercepted here; Default falls through to the activity.
  {
    using BA = CrossPointSettings::BUTTON_ACTION;
    using B = MappedInputManager::Button;
    auto actionFor = [&](const ButtonEventManager::ButtonEvent& ev) -> uint8_t {
      switch (ev.button) {
        case B::Back:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongBack;
          }
          break;
        case B::Confirm:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortConfirm;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleConfirm;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongConfirm;
          }
          break;
        case B::Left:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortLeft;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleLeft;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongLeft;
          }
          break;
        case B::Right:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortRight;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleRight;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongRight;
          }
          break;
        case B::PageBack:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageBack;
          }
          break;
        case B::PageForward:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageForward;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageForward;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageForward;
          }
          break;
        case B::Power:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPower;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePower;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPower;
          }
          break;
        default:
          break;  // Up/Down have no FSMs — ButtonEventManager never emits these
      }
      return BA::BTN_DEFAULT;
    };
    ButtonEventManager::ButtonEvent ev;
    std::vector<ButtonEventManager::ButtonEvent> defaultEvents;
    defaultEvents.reserve(8);
    while (buttonEventManager.consumeEvent(ev)) {
      const uint8_t action = actionFor(ev);
      if (action == BA::BTN_DEFAULT) {
        if (ev.type == ButtonEventManager::PressType::Double) {
          continue;
        }
        defaultEvents.push_back(ev);
        continue;
      }

      switch (static_cast<BA>(action)) {
        case BA::BTN_PAGE_FORWARD:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD);
          break;
        case BA::BTN_PAGE_BACK:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK);
          break;
        case BA::BTN_PAGE_FORWARD_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD_10);
          break;
        case BA::BTN_PAGE_BACK_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK_10);
          break;
        case BA::BTN_GO_HOME:
          activityManager.goHome();
          break;
        case BA::BTN_SLEEP:
          activityManager.goToSleep();
          break;
        case BA::BTN_FORCE_REFRESH: {
          RenderLock lock;
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
          break;
        }
        case BA::BTN_OPEN_TOC:
          activityManager.dispatchButtonAction(BA::BTN_OPEN_TOC);
          break;
        case BA::BTN_OPEN_BOOKMARKS:
          activityManager.goToGlobalBookmarks();
          break;
        case BA::BTN_STAR_PAGE:
          activityManager.dispatchButtonAction(BA::BTN_STAR_PAGE);
          break;
        case BA::BTN_FOOTNOTES:
          activityManager.dispatchButtonAction(BA::BTN_FOOTNOTES);
          break;
        case BA::BTN_NEXT_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_NEXT_SECTION);
          break;
        case BA::BTN_PREV_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_PREV_SECTION);
          break;
        case BA::BTN_EXIT_READER:
          activityManager.dispatchButtonAction(BA::BTN_EXIT_READER);
          break;
        case BA::BTN_READER_MENU:
          activityManager.dispatchButtonAction(BA::BTN_READER_MENU);
          break;
        case BA::BTN_KOREADER_SYNC:
          activityManager.dispatchButtonAction(BA::BTN_KOREADER_SYNC);
          break;
        default:
          break;
      }
    }

    for (auto it = defaultEvents.rbegin(); it != defaultEvents.rend(); ++it) {
      buttonEventManager.pushEventFront(it->button, it->type);
    }
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
