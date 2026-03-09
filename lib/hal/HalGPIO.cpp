#include <HalGPIO.h>
#include <SPI.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(UART0_RXD, INPUT);  // USB D-/UART0_RXD — do not use PULLDOWN (breaks USB enumeration)
  // Seed the debounce cache from the current pin state so isUsbConnected()
  // returns the correct value immediately after begin(), before update() runs.
  usbConnectedDebounced_ = (digitalRead(UART0_RXD) == HIGH);
}

void HalGPIO::update() {
  inputMgr.update();

  // Debounce USB connection state: immediately accept HIGH (connected), but require
  // USB_DEBOUNCE_COUNT consecutive LOW reads before declaring disconnected.
  // This prevents false "disconnected" readings when GPIO20 floats after USB removal.
  const bool rawHigh = (digitalRead(UART0_RXD) == HIGH);
  if (rawHigh) {
    usbConnectedDebounced_ = true;
    usbLowCount_ = 0;
  } else {
    if (usbLowCount_ < USB_DEBOUNCE_COUNT) {
      usbLowCount_++;
    }
    if (usbLowCount_ >= USB_DEBOUNCE_COUNT) {
      usbConnectedDebounced_ = false;
    }
  }
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

bool HalGPIO::isUsbConnected() const {
  // Returns debounced USB state (see update() for logic).
  // Immediately true when USB plugged in; requires USB_DEBOUNCE_COUNT ticks after unplug.
  return usbConnectedDebounced_;
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