#pragma once
#include <esp32-hal-tinyusb.h>
struct usbd_class_driver_t {
  const char* name;
  void (*init)();
  bool (*deinit)();
  void (*reset)(uint8_t);
  uint16_t (*open)(uint8_t, const tusb_desc_interface_t*, uint16_t);
  bool (*control_xfer_cb)(uint8_t, uint8_t, const tusb_control_request_t*);
  bool (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t);
  bool (*xfer_isr)(uint8_t, uint8_t, xfer_result_t, uint32_t);
  void (*sof)(uint8_t, uint32_t);
};
