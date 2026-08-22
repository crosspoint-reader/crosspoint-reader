#include "TerminalUsb.h"

#include <HalGPIO.h>
#include <Logging.h>

void TerminalUsb::begin() {
  // The dedicated knietty environment omits ENABLE_SERIAL_LOG, so setup() does
  // not initialize the shared HWCDC transport. Starting it here keeps normal
  // CrossPoint builds and their logging behavior unchanged.
  logSerial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);
#endif
}

int TerminalUsb::available() const { return logSerial.available(); }

int TerminalUsb::read() const { return logSerial.read(); }

size_t TerminalUsb::write(const uint8_t* bytes, const size_t size) const {
  if (!gpio.isUsbConnected()) {
    return 0;
  }
  return logSerial.write(bytes, size);
}

size_t TerminalUsb::write(const uint8_t byte) const { return write(&byte, 1); }

bool TerminalUsb::isConnected() const {
  // HWCDC exposes no dependable DTR accessor on ESP32-C3, so this deliberately
  // reports the HAL's link state rather than PTY-open state.
  return gpio.isUsbConnected();
}
