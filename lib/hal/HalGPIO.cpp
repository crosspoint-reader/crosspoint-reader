#include <HalGPIO.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  // X3 boards bias GPIO4 (EPD DC) around ~700 ADC counts at boot in our setup.
  // X4 boards do not, and use GPIO0 for battery ADC.
  _detectAdcValue = analogRead(4);
  _deviceType = (_detectAdcValue > 500 && _detectAdcValue < 1200) ? DeviceType::X3 : DeviceType::X4;
  _batteryPin = (_deviceType == DeviceType::X3) ? 4 : BAT_GPIO0;

  pinMode(_batteryPin, INPUT);
  pinMode(UART0_RXD, INPUT);

  // I2C init must come AFTER pinMode(UART0_RXD) because GPIO20 is shared
  // between USB detection (digital read) and I2C SDA. Wire.begin()
  // reconfigures the pin for I2C, so it must run last.
  if (_deviceType == DeviceType::X3) {
    Wire.begin(20, 0, 400000);
    _useI2C = true;
    _i2cAddr = 0x55;
    _socRegister = 0x2C;
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

int HalGPIO::getBatteryPercentage() const {
  if (_useI2C) {
    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // Returns 0 on I2C error so the UI shows 0% rather than crashing.
    Wire.beginTransmission(_i2cAddr);
    Wire.write(_socRegister);
    if (Wire.endTransmission(false) != 0) return 0;
    Wire.requestFrom(_i2cAddr, (uint8_t)2);
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
