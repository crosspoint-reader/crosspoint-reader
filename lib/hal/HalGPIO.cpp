#include <HalGPIO.h>
#include <SPI.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(UART0_RXD, INPUT);
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  const uint8_t bit = static_cast<uint8_t>(1u << buttonIndex);
  if (virtualButtonMask & bit) {
    virtualButtonMask &= static_cast<uint8_t>(~bit);
    return true;
  }
  return inputMgr.wasPressed(buttonIndex);
}

bool HalGPIO::wasAnyPressed() const { return virtualButtonMask != 0 || inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  const uint8_t bit = static_cast<uint8_t>(1u << buttonIndex);
  if (virtualButtonMask & bit) {
    virtualButtonMask &= static_cast<uint8_t>(~bit);
    return true;
  }
  return inputMgr.wasReleased(buttonIndex);
}

bool HalGPIO::wasAnyReleased() const { return virtualButtonMask != 0 || inputMgr.wasAnyReleased(); }

void HalGPIO::injectVirtualButton(uint8_t buttonIndex) { virtualButtonMask |= static_cast<uint8_t>(1u << buttonIndex); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

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