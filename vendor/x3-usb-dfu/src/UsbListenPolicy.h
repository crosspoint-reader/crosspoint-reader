#pragma once
#include <cstdint>
namespace usb_update {
// TinyUSB sets its configuration flag while handling SETUP. The host has not
// accepted the device until it ACKs the final zero-length IN status packet.
class ConfigurationAck {
  bool pending = false, next = false, confirmed = false;

 public:
  void reset() { pending = next = confirmed = false; }
  void setup(uint8_t type, uint8_t request, uint16_t value, uint16_t length) {
    pending = type == 0 && request == 9 && length == 0;
    if (pending) {
      next = value != 0;
      confirmed = false;
    }
  }
  void statusAck() {
    if (pending) confirmed = next;
    pending = false;
  }
  bool configured(bool mounted) const { return mounted && confirmed; }
};
constexpr uint32_t listenWindowMs(bool background, bool configured, bool started, uint32_t quietMs) {
  return background && configured && !started && quietMs >= 250 ? 8 : 100;
}
constexpr bool enumerationExpired(bool configured, uint32_t elapsedMs) { return !configured && elapsedMs >= 15000; }
}  // namespace usb_update
