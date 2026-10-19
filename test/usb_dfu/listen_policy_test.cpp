#include <cassert>

#include "UsbListenPolicy.h"
int main() {
  using namespace usb_update;
  ConfigurationAck link;
  assert(!link.configured(false) && !link.configured(true));
  link.setup(0x80, 6, 0x100, 18);  // Reading a descriptor is not configuration.
  link.statusAck();
  assert(!link.configured(true));
  link.setup(0, 9, 1, 0);
  assert(!link.configured(true));  // TinyUSB mounted before host ACK.
  link.statusAck();
  assert(link.configured(true) && !link.configured(false));
  link.setup(0xa1, 3, 0, 6);  // Ordinary DFU GETSTATUS retains confirmed state.
  link.statusAck();
  assert(link.configured(true));
  link.setup(0, 9, 0, 0);  // Deconfiguration clears the badge.
  link.statusAck();
  assert(!link.configured(true));
  link.setup(0, 9, 1, 0);
  link.setup(0x80, 6, 0x100, 18);  // Interrupted SET_CONFIGURATION cannot claim readiness.
  link.statusAck();
  assert(!link.configured(true));
  link.setup(0, 9, 1, 0);
  link.statusAck();
  link.reset();
  assert(!link.configured(true));
  assert(listenWindowMs(true, false, false, 1000) == 100);  // Enumeration gets the proven transfer cadence.
  assert(listenWindowMs(true, true, false, 249) == 100);    // Don't yield in a control exchange.
  assert(listenWindowMs(true, true, false, 250) == 8);      // Reader still gets time after enumeration.
  assert(listenWindowMs(true, true, true, 1000) == 100);    // Active flashing stays fast.
  assert(listenWindowMs(false, true, false, 1000) == 100);
  assert(!enumerationExpired(false, 14999));
  assert(enumerationExpired(false, 15000));
  assert(!enumerationExpired(true, 60000));  // An enumerated idle host is not a failed connection.
}
