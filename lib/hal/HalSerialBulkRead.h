#pragma once

#include <cstddef>
#include <cstdint>

// Lossless bulk reads from the USB serial console. HWCDC copies each received
// USB packet into a 256-byte queue from its interrupt and drops what does not
// fit, so a host that sends faster than the main loop reads loses bytes. While
// an instance exists that interrupt is masked and read() takes bytes straight
// from the 64-byte USB Serial/JTAG FIFO; when the FIFO is full the hardware
// NAKs the host, which waits. Use only on the task that runs the console.
//
// The interrupt belongs to arduino-esp32's HWCDC, which rewrites the whole enable
// register under a private lock whenever it arms TX (pinned 3.3.x core; recheck on
// an update). On an S3 that can undo our change from the other core, so the bit is
// checked and set again, and restoreReceiving() repairs a lost re-enable. On a
// UART console (no checked-in env) this only compiles: Arduino's RX buffer is all
// there is.
class HalSerialBulkRead {
 public:
  HalSerialBulkRead();
  ~HalSerialBulkRead();
  HalSerialBulkRead(const HalSerialBulkRead&) = delete;
  HalSerialBulkRead& operator=(const HalSerialBulkRead&) = delete;

  // Up to `size` bytes that have arrived, without waiting; 0 when none.
  size_t read(uint8_t* dst, size_t size);

  // Turns the receive interrupt back on if an instance left it off. Call while none exists.
  static void restoreReceiving();
  // False once the USB cable is gone (HWCDC drops what is written then).
  static bool cablePresent();
};
