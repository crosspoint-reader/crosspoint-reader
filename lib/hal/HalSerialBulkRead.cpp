#include "HalSerialBulkRead.h"

#include <Logging.h>

#include <algorithm>

#if LOG_SERIAL_IS_HWCDC
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <hal/usb_serial_jtag_ll.h>

namespace {

// HWCDC's interrupt runs on this core; the critical section keeps it out of the
// read-modify-write of the enable register it shares with us.
portMUX_TYPE intrMaskMux = portMUX_INITIALIZER_UNLOCKED;
// Once an instance has masked the interrupt, restoreReceiving() keeps it on;
// before that it leaves HWCDC's own setting alone.
bool maskedBefore = false;

bool receiveInterruptEnabled() {
  return usb_serial_jtag_ll_get_intr_ena_status() & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT;
}

// On an S3, a log write on the other core can rewrite the register with a stale
// copy of our bit just after we change it, so set it until it reads back.
void setReceiveInterrupt(bool enabled) {
  for (int attempt = 0; attempt < 4 && receiveInterruptEnabled() != enabled; attempt++) {
    portENTER_CRITICAL(&intrMaskMux);
    if (enabled) {
      usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    } else {
      usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    }
    portEXIT_CRITICAL(&intrMaskMux);
    esp_rom_delay_us(1);
  }
}

}  // namespace

HalSerialBulkRead::HalSerialBulkRead() {
  maskedBefore = true;
  setReceiveInterrupt(false);
}

HalSerialBulkRead::~HalSerialBulkRead() { setReceiveInterrupt(true); }

void HalSerialBulkRead::restoreReceiving() {
  if (maskedBefore && !receiveInterruptEnabled()) setReceiveInterrupt(true);
}

bool HalSerialBulkRead::cablePresent() { return HWCDC::isPlugged(); }

size_t HalSerialBulkRead::read(uint8_t* dst, size_t size) {
  // Re-masks if a clash re-enabled the interrupt since the last read.
  if (receiveInterruptEnabled()) setReceiveInterrupt(false);
  // Bytes HWCDC queued before the interrupt was masked come first.
  const int queued = logSerial.available();
  if (queued > 0) return logSerial.read(dst, std::min(static_cast<size_t>(queued), size));
  return usb_serial_jtag_ll_read_rxfifo(dst, size);
}

#else

HalSerialBulkRead::HalSerialBulkRead() = default;
HalSerialBulkRead::~HalSerialBulkRead() = default;
void HalSerialBulkRead::restoreReceiving() {}
bool HalSerialBulkRead::cablePresent() { return true; }

size_t HalSerialBulkRead::read(uint8_t* dst, size_t size) {
  const int ready = logSerial.available();
  return ready > 0 ? logSerial.read(dst, std::min(static_cast<size_t>(ready), size)) : 0;
}

#endif
