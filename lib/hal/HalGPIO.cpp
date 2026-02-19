#include <HalGPIO.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  // X3 boards bias GPIO4 (EPD DC) around ~700 ADC counts at boot in our setup.
  // X4 boards do not, and use GPIO0 for battery ADC.
  _detectAdcValue = analogRead(4);
  _deviceType = (_detectAdcValue > 500 && _detectAdcValue < 1200) ? DeviceType::X3 : DeviceType::X4;
  _batteryPin = deviceIsX3() ? 4 : BAT_GPIO0;

  pinMode(_batteryPin, INPUT);
  pinMode(UART0_RXD, INPUT);

  // I2C init must come AFTER pinMode(UART0_RXD) because GPIO20 is shared
  // between USB detection (digital read) and I2C SDA. Wire.begin()
  // reconfigures the pin for I2C, so it must run last.
  if (deviceIsX3()) {
    Wire.begin(20, 0, 400000);
    _batteryUseI2C = true;
    _batteryI2cAddr = 0x55;
    _batterySocRegister = 0x2C;
  }
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

int HalGPIO::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // Returns 0 on I2C error so the UI shows 0% rather than crashing.
    Wire.beginTransmission(_batteryI2cAddr);
    Wire.write(_batterySocRegister);
    if (Wire.endTransmission(false) != 0) return 0;
    Wire.requestFrom(_batteryI2cAddr, (uint8_t)2);
    if (Wire.available() < 2) return 0;
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    return soc > 100 ? 100 : soc;
  }
  static const BatteryMonitor bat(BAT_GPIO0);
  return bat.readPercentage();
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
