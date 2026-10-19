#pragma once
#include <esp32-hal-tinyusb.h>
inline std::atomic<unsigned> completions{0};
inline void dfu_moded_init() {}
inline bool dfu_moded_deinit() { return true; }
inline void dfu_moded_reset(uint8_t) {}
inline uint16_t dfu_moded_open(uint8_t, const tusb_desc_interface_t*, uint16_t n) { return n; }
inline bool dfu_moded_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t*) { return true; }
inline void tud_dfu_finish_flashing(uint8_t) { ++completions; }
