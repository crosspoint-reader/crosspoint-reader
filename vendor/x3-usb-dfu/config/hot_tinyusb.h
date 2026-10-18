#pragma once
#ifndef __ASSEMBLER__
#include <esp_attr.h>

#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "tusb.h"
#ifdef __cplusplus
extern "C" {
#endif
// Keep the per-packet queue/control dispatcher out of the flash cache shared
// with the locked USB bit engine. No upstream source or protocol is changed.
void IRAM_ATTR tud_task_ext(uint32_t timeout_ms, bool in_isr);
void IRAM_ATTR dcd_event_handler(dcd_event_t const* event, bool in_isr);
bool IRAM_ATTR usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr);

#ifdef __cplusplus
}
#endif
#endif
