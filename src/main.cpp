#include <Arduino.h>
#include <EInkDisplay.h>
#include <EpdRenderer.h>
#include <Epub.h>
#include <InputManager.h>
#include <SD.h>
#include <SPI.h>

#include "Battery.h"
#include "CrossPointState.h"
#include "screens/BootLogoScreen.h"
#include "screens/EpubReaderScreen.h"
#include "screens/FileSelectionScreen.h"
#include "screens/FullScreenMessageScreen.h"
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
EpdRenderer renderer(einkDisplay);
Screen* currentScreen;
CrossPointState appState;

// Power button timing
// Time required to confirm boot from sleep
constexpr unsigned long POWER_BUTTON_WAKEUP_MS = 1000;
// Time required to enter sleep mode
constexpr unsigned long POWER_BUTTON_SLEEP_MS = 1000;

Epub* loadEpub(const std::string& path) {
  if (!SD.exists(path.c_str())) {
    Serial.printf("File does not exist: %s\n", path.c_str());
    return nullptr;
  }

  const auto epub = new Epub(path, "/.crosspoint");
  if (epub->load()) {
    return epub;
  }

  Serial.println("Failed to load epub");
  delete epub;
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

  Serial.println("Verifying power button press");
  inputManager.update();
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(50);
    inputManager.update();
    Serial.println("Waiting...");
  }

  Serial.printf("Made it? %s\n", inputManager.isPressed(InputManager::BTN_POWER) ? "yes" : "no");
  if (inputManager.isPressed(InputManager::BTN_POWER)) {
    do {
      delay(50);
      inputManager.update();
    } while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < POWER_BUTTON_WAKEUP_MS);
    abort = inputManager.getHeldTime() < POWER_BUTTON_WAKEUP_MS;
  } else {
    abort = true;
  }

  Serial.printf("held for %lu\n", inputManager.getHeldTime());

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

  Serial.println("Power button released after a long press. Entering deep sleep.");
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

  Epub* epub = loadEpub(path);
  if (epub) {
    appState.openEpubPath = path;
    appState.saveToFile();
    exitScreen();
    enterNewScreen(new EpubReaderScreen(renderer, inputManager, epub, onGoHome));
  } else {
    exitScreen();
    enterNewScreen(new FullScreenMessageScreen(renderer, inputManager, "Failed to load epub", REGULAR, EInkDisplay::HALF_REFRESH));
    delay(2000);
    onGoHome();
  }
}

void onGoHome() {
  exitScreen();
  enterNewScreen(new FileSelectionScreen(renderer, inputManager, onSelectEpubFile));
}

void setup() {
  inputManager.begin();
  verifyWakeupLongPress();

  // Begin serial only if USB connected
  pinMode(UART0_RXD, INPUT);
  if (digitalRead(UART0_RXD) == HIGH) {
    Serial.begin(115200);
  }

  // Initialize pins
  pinMode(BAT_GPIO0, INPUT);

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK, SD_SPI_MISO, EPD_MOSI, EPD_CS);

  // Initialize display
  einkDisplay.begin();
  Serial.println("Display initialized");

  exitScreen();
  enterNewScreen(new BootLogoScreen(renderer, inputManager));

  // SD Card Initialization
  SD.begin(SD_SPI_CS, SPI, SPI_FQ);

  appState.loadFromFile();
  if (!appState.openEpubPath.empty()) {
    Epub* epub = loadEpub(appState.openEpubPath);
    if (epub) {
      exitScreen();
      enterNewScreen(new EpubReaderScreen(renderer, inputManager, epub, onGoHome));
      // Ensure we're not still holding the power button before leaving setup
      waitForPowerRelease();
      return;
    }
  }

  exitScreen();
  enterNewScreen(new FileSelectionScreen(renderer, inputManager, onSelectEpubFile));

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  delay(10);

  static unsigned long lastMemPrint = 0;
  if (Serial && millis() - lastMemPrint >= 5000) {
    Serial.printf("[%lu] Memory - Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  inputManager.update();
  if (inputManager.wasReleased(InputManager::BTN_POWER) && inputManager.getHeldTime() > POWER_BUTTON_WAKEUP_MS) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (currentScreen) {
    currentScreen->handleInput();
  }
}
