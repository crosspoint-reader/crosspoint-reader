#include <cassert>
#include <cstdio>

#include "Memory.h"
#include "UsbWorkspace.h"
#include "device/dcd.h"
#include "softusb_dcd.h"

static bool connected;
extern "C" {
bool su_phy_init() {
  connected = true;
  return true;
}
void su_phy_deinit() { connected = false; }
void su_phy_connect(bool value) { connected = value; }
void su_phy_irq(bool) {}
void su_phy_interrupt() {}
void dcd_event_handler(const dcd_event_t*, bool) {}
}

int main() {
  const tusb_rhport_init_t init{TUSB_ROLE_DEVICE, TUSB_SPEED_FULL};
  assert(!dcd_init(0, &init));
  for (unsigned session = 0; session < 32; ++session) {
    auto workspace = makeUniqueNoThrow<usb_update::UsbWorkspace>();
    assert(workspace);
    for (auto value : workspace->tokenCrc) assert(value == 0);
    for (auto value : workspace->transfer) assert(value == 0);
    assert(reinterpret_cast<uintptr_t>(workspace->transfer) % 4 == 0);
    su_dcd_bind_device(&workspace->device);
    assert(su_dcd_device() == &workspace->device);
    assert(dcd_init(0, &init) && connected);
    assert(su_dcd_device()->address == 0);
    assert(dcd_edpt_xfer(0, 0, workspace->transfer, 64, false));
    su_dcd_device()->address = 37;
    assert(dcd_deinit(0) && !connected);
    assert(su_dcd_device()->address == 0);
    su_dcd_bind_device(nullptr);
    assert(su_dcd_device() == nullptr);
    assert(!dcd_init(0, &init));
  }
  puts("PASS: 32 connection workspace lifetimes with production software DCD, aligned buffers and reset state");
}
