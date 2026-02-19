#include <HalGPIO.h>
#include <SPI.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace {
int readAveragedAdc(uint8_t pin, uint8_t samples = 8) {
  long sum = 0;
  for (uint8_t i = 0; i < samples; ++i) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return static_cast<int>(sum / samples);
}
}  // namespace

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  // X3 routes battery sense on GPIO4, X4 routes battery sense on GPIO0.
  // Read both channels and classify by relative dominance; this is more stable
  // than using a single floating-channel threshold.
  const int adc4 = readAveragedAdc(4);
  const int adc0 = readAveragedAdc(BAT_GPIO0);
  _detectAdcValue = adc4;

  static constexpr int kDominanceMargin = 150;
  static constexpr int kX3Min = 500;
  static constexpr int kX3Max = 1500;
  const bool adc4LooksX3Range = (adc4 >= kX3Min && adc4 <= kX3Max);
  const bool adc4Dominant = (adc4 > adc0 + kDominanceMargin);
  const bool adc0Dominant = (adc0 > adc4 + kDominanceMargin);

  if (adc4LooksX3Range && adc4Dominant) {
    _deviceType = DeviceType::X3;
  } else if (adc0Dominant) {
    _deviceType = DeviceType::X4;
  } else {
    // Conservative fallback: X4 avoids enabling X3-only display behavior on
    // ambiguous reads, which is safer for refresh correctness.
    _deviceType = DeviceType::X4;
  }

  _batteryPin = deviceIsX3() ? 4 : BAT_GPIO0;

  pinMode(_batteryPin, INPUT);
  pinMode(UART0_RXD, INPUT);
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  if (deviceIsX3()) {
    // X3: Direct GPIO read (inputMgr not yet reliable at this point)
    const uint8_t powerPin = InputManager::POWER_BUTTON_PIN;
    if (digitalRead(powerPin) != LOW) {
      startDeepSleep();
    }
    const unsigned long holdStart = millis();
    while (millis() - holdStart < calibratedDuration) {
      if (digitalRead(powerPin) != LOW) {
        startDeepSleep();
      }
      delay(5);
    }
  } else {
    // X4: Use inputMgr with wait window for it to stabilize
    const auto start = millis();
    inputMgr.update();
    // inputMgr.isPressed() may take up to ~500ms to return correct state
    while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
      delay(10);
      inputMgr.update();
    }
    if (inputMgr.isPressed(BTN_POWER)) {
      do {
        delay(10);
        inputMgr.update();
      } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getHeldTime() < calibratedDuration);
      if (inputMgr.getHeldTime() < calibratedDuration) {
        startDeepSleep();
      }
    } else {
      startDeepSleep();
    }
  }
}

bool HalGPIO::isUsbConnected() const {
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
