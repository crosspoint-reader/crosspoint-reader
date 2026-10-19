#include "device/dcd.h"

#include "softusb_dcd.h"

_Static_assert(CFG_TUD_ENDPOINT0_SIZE == SU_MPS, "software DCD requires 64-byte EP0");
_Static_assert(CFG_TUD_ENDPOINT0_BUFSIZE == SU_MPS, "software DCD queues one packet per transfer");
static su_device* activeDevice;
#define device (*activeDevice)
void su_dcd_bind_device(su_device* value) { activeDevice = value; }
su_device* su_dcd_device(void) { return activeDevice; }
void su_dcd_service(void) {
  su_event event;
  while (su_next_event(&device, &event)) {
    if (event.type == SU_EVENT_RESET)
      dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    else if (event.type == SU_EVENT_SETUP)
      dcd_event_setup_received(0, event.setup, false);
    else
      dcd_event_xfer_complete(0, event.ep, event.len, XFER_RESULT_SUCCESS, false);
  }
}
bool dcd_init(uint8_t p, const tusb_rhport_init_t* init) {
  if (!activeDevice || p || init->speed != TUSB_SPEED_FULL) return false;
  su_device_init(&device);
  return su_phy_init();
}
bool dcd_deinit(uint8_t p) {
  (void)p;
  su_phy_deinit();
  su_device_init(&device);
  return true;
}
void dcd_int_enable(uint8_t p) {
  (void)p;
  su_phy_irq(true);
}
void dcd_int_disable(uint8_t p) {
  (void)p;
  su_phy_irq(false);
}
void dcd_int_handler(uint8_t p) {
  (void)p;
  su_phy_interrupt();
}
void dcd_connect(uint8_t p) {
  (void)p;
  su_phy_connect(true);
}
void dcd_disconnect(uint8_t p) {
  (void)p;
  su_phy_connect(false);
}
void dcd_remote_wakeup(uint8_t p) { (void)p; }  // Not advertised in descriptors.
void dcd_sof_enable(uint8_t p, bool enable) {
  (void)p;
  (void)enable;
}
void dcd_edpt_close_all(uint8_t p) { (void)p; }  // Only EP0 exists.
bool dcd_edpt_open(uint8_t p, const tusb_desc_endpoint_t* desc) {
  (void)p;
  (void)desc;
  return false;
}
bool dcd_edpt_iso_alloc(uint8_t p, uint8_t ep, uint16_t size) {
  (void)p;
  (void)ep;
  (void)size;
  return false;
}
bool dcd_edpt_iso_activate(uint8_t p, const tusb_desc_endpoint_t* desc) {
  (void)p;
  (void)desc;
  return false;
}
SU_HOT bool dcd_edpt_xfer(uint8_t p, uint8_t ep, uint8_t* buf, uint16_t len, bool isr) {
  (void)isr;
  return p == 0 && su_submit(&device, ep, buf, len);
}
void dcd_set_address(uint8_t p, uint8_t address) {
  if (p || !su_set_address(&device, address)) su_stall(&device, 0x80);
}
void dcd_edpt_stall(uint8_t p, uint8_t ep) {
  if (!p) su_stall(&device, ep);
}
void dcd_edpt_clear_stall(uint8_t p, uint8_t ep) {
  // TinyUSB clears EP0 stalls by receiving SETUP, not this non-control API.
  (void)p;
  (void)ep;
}
