#include <Arduino.h>
#include <EInkDisplay.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>
#include <SPI.h>
#include <builtinFonts/bookerly_2b.h>
#include <builtinFonts/bookerly_bold_2b.h>
#include <builtinFonts/bookerly_bold_italic_2b.h>
#include <builtinFonts/bookerly_italic_2b.h>
#include <builtinFonts/pixelarial14.h>
#include <builtinFonts/ubuntu_10.h>
#include <builtinFonts/ubuntu_bold_10.h>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "config.h"
#include "screens/BootLogoScreen.h"
#include "screens/EpubReaderScreen.h"
#include "screens/FileSelectionScreen.h"
#include "screens/FullScreenMessageScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/SleepScreen.h"

#define SPI_FQ 40000000
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define UART0_RXD 20  // Used for USB connection detection

#define SD_SPI_CS 12
#define SD_SPI_MISO 7

EInkDisplay einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
InputManager inputManager;
GfxRenderer renderer(einkDisplay);
Screen* currentScreen;
CrossPointState appState;

// Fonts
EpdFont bookerlyFont(&bookerly_2b);
EpdFont bookerlyBoldFont(&bookerly_bold_2b);
EpdFont bookerlyItalicFont(&bookerly_italic_2b);
EpdFont bookerlyBoldItalicFont(&bookerly_bold_italic_2b);
EpdFontFamily bookerlyFontFamily(&bookerlyFont, &bookerlyBoldFont, &bookerlyItalicFont, &bookerlyBoldItalicFont);

EpdFont smallFont(&pixelarial14);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ubuntu10Font(&ubuntu_10);
EpdFont ubuntuBold10Font(&ubuntu_bold_10);
EpdFontFamily ubuntuFontFamily(&ubuntu10Font, &ubuntuBold10Font);

// Power button timing
// Time required to confirm boot from sleep
constexpr unsigned long POWER_BUTTON_WAKEUP_MS = 500;
// Time required to enter sleep mode
constexpr unsigned long POWER_BUTTON_SLEEP_MS = 500;
// Auto-sleep timeout (10 minutes of inactivity)
constexpr unsigned long AUTO_SLEEP_TIMEOUT_MS = 10 * 60 * 1000;

std::unique_ptr<Epub> loadEpub(const std::string& path) {
  if (!SD.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  Serial.printf("[%lu] [   ] Failed to load epub\n", millis());
  return nullptr;
}

void exitScreen() {
  if (currentScreen) {
    currentScreen->onExit();
    delete currentScreen;
  }
}

void enterNewScreen(Screen* screen) {
  currentScreen = screen;
  currentScreen->onEnter();
}

// Verify long press on wake-up from deep sleep
void verifyWakeupLongPress() {
  // Give the user up to 1000ms to start holding the power button, and must hold for POWER_BUTTON_WAKEUP_MS
  const auto start = millis();
  bool abort = false;

  Serial.printf("[%lu] [   ] Verifying power button press\n", millis());
  inputManager.update();
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(50);
    inputManager.update();
  }

  if (inputManager.isPressed(InputManager::BTN_POWER)) {
    do {
      delay(50);
      inputManager.update();
    } while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < POWER_BUTTON_WAKEUP_MS);
    abort = inputManager.getHeldTime() < POWER_BUTTON_WAKEUP_MS;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
}

void waitForPowerRelease() {
  inputManager.update();
  while (inputManager.isPressed(InputManager::BTN_POWER)) {
    delay(50);
    inputManager.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  exitScreen();
  enterNewScreen(new SleepScreen(renderer, inputManager));

  Serial.printf("[%lu] [   ] Power button released after a long press. Entering deep sleep.\n", millis());
  delay(1000);  // Allow Serial buffer to empty and display to update

  // Enable Wakeup on LOW (button press)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

  einkDisplay.deepSleep();

  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void onGoHome();
void onSelectEpubFile(const std::string& path) {
  exitScreen();
  enterNewScreen(new FullScreenMessageScreen(renderer, inputManager, "Loading..."));

  auto epub = loadEpub(path);
  if (epub) {
    appState.openEpubPath = path;
    appState.saveToFile();
    exitScreen();
    enterNewScreen(new EpubReaderScreen(renderer, inputManager, std::move(epub), onGoHome));
  } else {
    exitScreen();
    enterNewScreen(
        new FullScreenMessageScreen(renderer, inputManager, "Failed to load epub", REGULAR, EInkDisplay::HALF_REFRESH));
    delay(2000);
    onGoHome();
  }
}

void onGoToSettings() {
  exitScreen();
  enterNewScreen(new SettingsScreen(renderer, inputManager, onGoHome));
}

void onGoHome() {
  exitScreen();
  enterNewScreen(new FileSelectionScreen(renderer, inputManager, onSelectEpubFile, onGoToSettings));
}

void setup() {
  Serial.begin(115200);

  Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  inputManager.begin();
  verifyWakeupLongPress();

  // Initialize pins
  pinMode(BAT_GPIO0, INPUT);

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK, SD_SPI_MISO, EPD_MOSI, EPD_CS);

  // Initialize display
  einkDisplay.begin();
  Serial.printf("[%lu] [   ] Display initialized\n", millis());

  renderer.insertFont(READER_FONT_ID, bookerlyFontFamily);
  renderer.insertFont(UI_FONT_ID, ubuntuFontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  Serial.printf("[%lu] [   ] Fonts setup\n", millis());

  exitScreen();
  enterNewScreen(new BootLogoScreen(renderer, inputManager));

  // SD Card Initialization
  SD.begin(SD_SPI_CS, SPI, SPI_FQ);

  SETTINGS.loadFromFile();
  appState.loadFromFile();
  if (!appState.openEpubPath.empty()) {
    auto epub = loadEpub(appState.openEpubPath);
    if (epub) {
      exitScreen();
      enterNewScreen(new EpubReaderScreen(renderer, inputManager, std::move(epub), onGoHome));
      // Ensure we're not still holding the power button before leaving setup
      waitForPowerRelease();
      return;
    }
  }

  exitScreen();
  enterNewScreen(new FileSelectionScreen(renderer, inputManager, onSelectEpubFile, onGoToSettings));

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  delay(10);

  static unsigned long lastMemPrint = 0;
  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  inputManager.update();

  // Check for any user activity (button press or release)
  static unsigned long lastActivityTime = millis();
  if (inputManager.wasAnyPressed() || inputManager.wasAnyReleased()) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  if (millis() - lastActivityTime >= AUTO_SLEEP_TIMEOUT_MS) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), AUTO_SLEEP_TIMEOUT_MS);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (inputManager.wasReleased(InputManager::BTN_POWER) && inputManager.getHeldTime() > POWER_BUTTON_WAKEUP_MS) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (currentScreen) {
    currentScreen->handleInput();
  }
}
