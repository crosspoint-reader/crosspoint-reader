#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <FontPartition.h>

#include <cstring>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
Activity* currentActivity;

// Font helpers — stable EpdFont pointers from FontPartition, no heap allocation
static EpdFontFamily makeFontFamily(const char* regular, const char* bold = nullptr,
                                    const char* italic = nullptr, const char* boldItalic = nullptr) {
  return EpdFontFamily(FontPartition::getEpdFont(regular),
                       bold ? FontPartition::getEpdFont(bold) : nullptr,
                       italic ? FontPartition::getEpdFont(italic) : nullptr,
                       boldItalic ? FontPartition::getEpdFont(boldItalic) : nullptr);
}

static const char* fontGroupForFamily(uint8_t family) {
  switch (family) {
    case CrossPointSettings::NOTOSANS:      return "notosans";
    case CrossPointSettings::OPENDYSLEXIC:  return "opendyslexic";
    case CrossPointSettings::BOOKERLY:
    default:                                return "bookerly";
  }
}

void loadReaderFontsForCurrentSetting() {
  const char* group = fontGroupForFamily(SETTINGS.fontFamily);
  if (!FontPartition::loadReaderGroup(group)) {
    LOG_ERR("MAIN", "Failed to load reader font group: %s", group);
  }
}

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

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
    gpio.startDeepSleep();
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
  APP_STATE.lastSleepFromReader = currentActivity && currentActivity->isReaderActivity();
  APP_STATE.saveToFile();
  exitActivity();
  enterNewActivity(new SleepActivity(renderer, mappedInputManager));

  display.deepSleep();
  LOG_DBG("MAIN", "Power button press calibration value: %lu ms", t2 - t1);
  LOG_DBG("MAIN", "Entering deep sleep");

  gpio.startDeepSleep();
}

void onGoHome();
void onGoToMyLibraryWithPath(const std::string& path);
void onGoToRecentBooks();
void onGoToReader(const std::string& initialEpubPath) {
  loadReaderFontsForCurrentSetting();
  exitActivity();
  enterNewActivity(
      new ReaderActivity(renderer, mappedInputManager, initialEpubPath, onGoHome, onGoToMyLibraryWithPath));
}

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToMyLibrary() {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToRecentBooks() {
  exitActivity();
  enterNewActivity(new RecentBooksActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToMyLibraryWithPath(const std::string& path) {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, path));
}

void onGoToBrowser() {
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoHome() {
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager, onGoToReader, onGoToMyLibrary, onGoToRecentBooks,
                                    onGoToSettings, onGoToFileTransfer, onGoToBrowser));
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  LOG_DBG("MAIN", "Display initialized");

  if (!FontPartition::begin()) {
    LOG_ERR("MAIN", "Failed to load font partition!");
    return;
  }

  // UI fonts — always mapped
  renderer.insertFont(UI_10_FONT_ID, makeFontFamily("ubuntu_10_regular", "ubuntu_10_bold"));
  renderer.insertFont(UI_12_FONT_ID, makeFontFamily("ubuntu_12_regular", "ubuntu_12_bold"));
  renderer.insertFont(SMALL_FONT_ID, makeFontFamily("notosans_8_regular"));

  // Reader fonts — all registered once; underlying data reflects mmap state automatically
  renderer.insertFont(BOOKERLY_12_FONT_ID,
      makeFontFamily("bookerly_12_regular", "bookerly_12_bold", "bookerly_12_italic", "bookerly_12_bolditalic"));
  renderer.insertFont(BOOKERLY_14_FONT_ID,
      makeFontFamily("bookerly_14_regular", "bookerly_14_bold", "bookerly_14_italic", "bookerly_14_bolditalic"));
  renderer.insertFont(BOOKERLY_16_FONT_ID,
      makeFontFamily("bookerly_16_regular", "bookerly_16_bold", "bookerly_16_italic", "bookerly_16_bolditalic"));
  renderer.insertFont(BOOKERLY_18_FONT_ID,
      makeFontFamily("bookerly_18_regular", "bookerly_18_bold", "bookerly_18_italic", "bookerly_18_bolditalic"));

  renderer.insertFont(NOTOSANS_12_FONT_ID,
      makeFontFamily("notosans_12_regular", "notosans_12_bold", "notosans_12_italic", "notosans_12_bolditalic"));
  renderer.insertFont(NOTOSANS_14_FONT_ID,
      makeFontFamily("notosans_14_regular", "notosans_14_bold", "notosans_14_italic", "notosans_14_bolditalic"));
  renderer.insertFont(NOTOSANS_16_FONT_ID,
      makeFontFamily("notosans_16_regular", "notosans_16_bold", "notosans_16_italic", "notosans_16_bolditalic"));
  renderer.insertFont(NOTOSANS_18_FONT_ID,
      makeFontFamily("notosans_18_regular", "notosans_18_bold", "notosans_18_italic", "notosans_18_bolditalic"));

  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID,
      makeFontFamily("opendyslexic_8_regular", "opendyslexic_8_bold", "opendyslexic_8_italic", "opendyslexic_8_bolditalic"));
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID,
      makeFontFamily("opendyslexic_10_regular", "opendyslexic_10_bold", "opendyslexic_10_italic", "opendyslexic_10_bolditalic"));
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID,
      makeFontFamily("opendyslexic_12_regular", "opendyslexic_12_bold", "opendyslexic_12_italic", "opendyslexic_12_bolditalic"));
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID,
      makeFontFamily("opendyslexic_14_regular", "opendyslexic_14_bold", "opendyslexic_14_italic", "opendyslexic_14_bolditalic"));

  LOG_DBG("MAIN", "Fonts setup (%d fonts)", FontPartition::fontCount());
}

void setup() {
  t1 = millis();

  gpio.begin();

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
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, "SD card error", EpdFontFamily::BOLD));
    return;
  }

  SETTINGS.loadFromFile();
  I18N.loadSettings();
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
      gpio.startDeepSleep();
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

  exitActivity();
  enterNewActivity(new BootActivity(renderer, mappedInputManager));

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
  // crashed (indicated by readerActivityLoadCount > 0)
  if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    onGoHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    onGoToReader(path);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes", ESP.getFreeHeap(), ESP.getHeapSize(),
            ESP.getMinFreeHeap());
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
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
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
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // 3 seconds
    if (millis() - lastActivityTime >= IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
