/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED || CFG_TUD_ENABLED

#include "tusb.h"
#include "common/tusb_private.h"

#if CFG_TUD_ENABLED
#include "device/usbd_pvt.h"
#endif

#if CFG_TUH_ENABLED
#include "host/usbh_pvt.h"
#endif

// Suppress IAR warning
// Warning[Pe111]: statement is unreachable
#if defined(__ICCARM__)
#pragma diag_suppress = Pe111
#endif

tusb_role_t _tusb_rhport_role[TUP_USBIP_CONTROLLER_NUM] = { TUSB_ROLE_INVALID };

//--------------------------------------------------------------------
// Weak/Default API, can be overwritten by Application
//--------------------------------------------------------------------

#if CFG_TUSB_OS != OPT_OS_NONE
TU_ATTR_WEAK uint32_t tusb_time_millis_api(void) {
  return osal_time_millis();
}

TU_ATTR_WEAK void tusb_time_delay_ms_api(uint32_t ms) {
  osal_task_delay(ms);
}

#else
// tusb_time_millis_api() must be implemented by user application.

TU_ATTR_WEAK void tusb_time_delay_ms_api(uint32_t ms) {
  // delay using millis()
  const uint32_t time_ms = tusb_time_millis_api();
  while ((tusb_time_millis_api() - time_ms) < ms) {}
}
#endif

TU_ATTR_WEAK void *tusb_app_virt_to_phys(void *virt_addr) {
  return virt_addr;
}

TU_ATTR_WEAK void* tusb_app_phys_to_virt(void *phys_addr) {
  return phys_addr;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  //  backward compatible called with tusb_init(void)
  #if defined(TUD_OPT_RHPORT) || defined(TUH_OPT_RHPORT)
  if (rh_init == NULL) {
    #if CFG_TUD_ENABLED && defined(TUD_OPT_RHPORT)
    // init device stack CFG_TUSB_RHPORTx_MODE must be defined
    const tusb_rhport_init_t dev_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUD_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL
    };
    TU_ASSERT ( tud_rhport_init(TUD_OPT_RHPORT, &dev_init) );
    _tusb_rhport_role[TUD_OPT_RHPORT] = TUSB_ROLE_DEVICE;
    #endif

    #if CFG_TUH_ENABLED && defined(TUH_OPT_RHPORT)
    // init host stack CFG_TUSB_RHPORTx_MODE must be defined
    const tusb_rhport_init_t host_init = {
      .role = TUSB_ROLE_HOST,
      .speed = TUH_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL
    };
    TU_ASSERT( tuh_rhport_init(TUH_OPT_RHPORT, &host_init) );
    _tusb_rhport_role[TUH_OPT_RHPORT] = TUSB_ROLE_HOST;
    #endif

    return true;
  }
  #endif

  // new API with explicit rhport and role
  TU_ASSERT(rhport < TUP_USBIP_CONTROLLER_NUM && rh_init->role != TUSB_ROLE_INVALID);
  _tusb_rhport_role[rhport] = rh_init->role;

  #if CFG_TUD_ENABLED
  if (rh_init->role == TUSB_ROLE_DEVICE) {
    TU_ASSERT(tud_rhport_init(rhport, rh_init));
  }
  #endif

  #if CFG_TUH_ENABLED
  if (rh_init->role == TUSB_ROLE_HOST) {
    TU_ASSERT(tuh_rhport_init(rhport, rh_init));
  }
  #endif

  return true;
}

bool tusb_inited(void) {
  bool ret = false;

  #if CFG_TUD_ENABLED
  if (tud_inited()) {
    ret = true;
  }
  #endif

  #if CFG_TUH_ENABLED
  if (tuh_inited()) {
    ret = true;
  }
  #endif

  return ret;
}

void tusb_int_handler(uint8_t rhport, bool in_isr) {
  TU_VERIFY(rhport < TUP_USBIP_CONTROLLER_NUM,);

  #if CFG_TUD_ENABLED
  if (_tusb_rhport_role[rhport] == TUSB_ROLE_DEVICE) {
    (void) in_isr;
    dcd_int_handler(rhport);
  }
  #endif

  #if CFG_TUH_ENABLED
  if (_tusb_rhport_role[rhport] == TUSB_ROLE_HOST) {
    hcd_int_handler(rhport, in_isr);
  }
  #endif
}

bool tusb_deinit(uint8_t rhport) {
  TU_VERIFY(rhport < TUP_USBIP_CONTROLLER_NUM);
  bool ret = false;

  #if CFG_TUD_ENABLED
  if (_tusb_rhport_role[rhport] == TUSB_ROLE_DEVICE) {
    TU_ASSERT(tud_deinit(rhport));
    _tusb_rhport_role[rhport] = TUSB_ROLE_INVALID;
    ret = true;
  }
  #endif

  #if CFG_TUH_ENABLED
  if (_tusb_rhport_role[rhport] == TUSB_ROLE_HOST) {
    TU_ASSERT(tuh_deinit(rhport));
    _tusb_rhport_role[rhport] = TUSB_ROLE_INVALID;
    ret = true;
  }
  #endif

  return ret;
}

//--------------------------------------------------------------------+
// Descriptor helper
//--------------------------------------------------------------------+

uint8_t const* tu_desc_find(uint8_t const* desc, uint8_t const* end, uint8_t byte1) {
  while (desc + 1 < end) {
    if (desc[1] == byte1) {
      return desc;
    }
    desc += desc[DESC_OFFSET_LEN];
  }
  return NULL;
}

uint8_t const* tu_desc_find2(uint8_t const* desc, uint8_t const* end, uint8_t byte1, uint8_t byte2) {
  while (desc + 2 < end) {
    if (desc[1] == byte1 && desc[2] == byte2) {
      return desc;
    }
    desc += desc[DESC_OFFSET_LEN];
  }
  return NULL;
}

uint8_t const* tu_desc_find3(uint8_t const* desc, uint8_t const* end, uint8_t byte1, uint8_t byte2, uint8_t byte3) {
  while (desc + 3 < end) {
    if (desc[1] == byte1 && desc[2] == byte2 && desc[3] == byte3) {
      return desc;
    }
    desc += desc[DESC_OFFSET_LEN];
  }
  return NULL;
}

//--------------------------------------------------------------------+
// Endpoint Helper for both Host and Device stack
//--------------------------------------------------------------------+

bool tu_edpt_claim(volatile uint8_t* ep_state, osal_mutex_t mutex) {
  (void) mutex;

  // pre-check to help reducing mutex lock
  TU_VERIFY((*ep_state & (TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED | TU_EDPT_STATE_RX_PENDING)) == 0);
  (void) osal_mutex_lock(mutex, OSAL_TIMEOUT_WAIT_FOREVER);

  // can only claim the endpoint if it is not busy, not claimed yet and its received data is consumed.
  bool const available = (*ep_state & (TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED | TU_EDPT_STATE_RX_PENDING)) == 0;
  if (available) {
    *ep_state |= TU_EDPT_STATE_CLAIMED;
  }

  (void) osal_mutex_unlock(mutex);
  return available;
}

bool tu_edpt_release(volatile uint8_t* ep_state, osal_mutex_t mutex) {
  (void) mutex;
  (void) osal_mutex_lock(mutex, OSAL_TIMEOUT_WAIT_FOREVER);

  // can only release the endpoint if it is claimed and not busy
  bool const ret = (*ep_state & (TU_EDPT_STATE_CLAIMED | TU_EDPT_STATE_BUSY)) == TU_EDPT_STATE_CLAIMED;
  if (ret) {
    *ep_state &= (uint8_t) ~TU_EDPT_STATE_CLAIMED;
  }

  (void) osal_mutex_unlock(mutex);
  return ret;
}

#if CFG_TUSB_DEBUG
bool tu_edpt_validate(const tusb_desc_endpoint_t *desc_ep, tusb_speed_t speed) {
  const uint16_t max_packet_size = tu_edpt_packet_size(desc_ep);
  TU_LOG2("  Open EP %02X with Size = %u\r\n", desc_ep->bEndpointAddress, max_packet_size);
  TU_ASSERT(max_packet_size > 0);

  switch (desc_ep->bmAttributes.xfer) {
    case TUSB_XFER_ISOCHRONOUS: {
      const uint16_t spec_size = (speed == TUSB_SPEED_HIGH ? 1024 : 1023);
      TU_ASSERT(max_packet_size <= spec_size);
      break;
    }

    case TUSB_XFER_BULK:
      if (speed == TUSB_SPEED_HIGH) {
        // Bulk highspeed must be EXACTLY 512
        TU_ASSERT(max_packet_size == 512);
      } else {
        // Bulk fullspeed can only be 8, 16, 32, 64
        TU_ASSERT(max_packet_size == 8 || max_packet_size == 16 || max_packet_size == 32 || max_packet_size == 64);
      }
      break;

    case TUSB_XFER_INTERRUPT: {
      const uint16_t spec_size = (speed == TUSB_SPEED_HIGH ? 1024 : 64);
      TU_ASSERT(max_packet_size <= spec_size);
      break;
    }

    default:
      return false;
  }

  return true;
}
#endif

bool tu_bind_driver_to_ep_itf(uint8_t driver_id, uint8_t ep2drv[][2], uint8_t itf2drv[], uint8_t itf_max,
                              uint8_t ep_max, const uint8_t *p_desc, uint16_t desc_len) {
  const uint8_t *desc_end = p_desc + desc_len;
  while (tu_desc_in_bounds(p_desc, desc_end)) {
    const uint8_t desc_type = tu_desc_type(p_desc);

    if (desc_type == TUSB_DESC_ENDPOINT) {
      const uint8_t ep_addr  = ((const tusb_desc_endpoint_t *)p_desc)->bEndpointAddress;
      const uint8_t ep_num   = tu_edpt_number(ep_addr);
      const uint8_t ep_dir   = tu_edpt_dir(ep_addr);
      TU_ASSERT(ep_num < ep_max);
      ep2drv[ep_num][ep_dir] = driver_id;
    } else if (desc_type == TUSB_DESC_INTERFACE) {
      const tusb_desc_interface_t *desc_itf = (const tusb_desc_interface_t *)p_desc;
      if (desc_itf->bAlternateSetting == 0) {
        TU_ASSERT(desc_itf->bInterfaceNumber < itf_max);
        itf2drv[desc_itf->bInterfaceNumber] = driver_id;
      }
    }

    p_desc = tu_desc_next(p_desc);
  }
  return true;
}

//--------------------------------------------------------------------+
// Endpoint Stream Helper for both Host and Device stack
//--------------------------------------------------------------------+

bool tu_edpt_stream_init(tu_edpt_stream_t *s, bool is_host, bool is_tx, bool overwritable, void *ff_buf,
                         uint16_t ff_bufsize, uint8_t *ep_buf) {
  (void) is_tx;

  if (ff_buf == NULL || ff_bufsize == 0) {
    return false;
  }

  s->is_host = is_host;
  tu_fifo_config(&s->ff, ff_buf, ff_bufsize, overwritable);

  #if OSAL_MUTEX_REQUIRED
  if (ff_buf != NULL && ff_bufsize > 0) {
    osal_mutex_t new_mutex = osal_mutex_create(&s->ff_mutexdef);
    tu_fifo_config_mutex(&s->ff, is_tx ? new_mutex : NULL, is_tx ? NULL : new_mutex);
  }
  #endif

  s->ep_buf = ep_buf;

  return true;
}

static bool stream_claim(tu_edpt_stream_t *s) {
  TU_VERIFY(s->ep_addr != 0); // must be opened
  if (s->is_host) {
    #if CFG_TUH_ENABLED
    return usbh_edpt_claim(s->hwid, s->ep_addr);
  #endif
  } else {
    #if CFG_TUD_ENABLED
    return usbd_edpt_claim(s->hwid, s->ep_addr);
  #endif
  }
  return false;
}

static bool stream_xfer(tu_edpt_stream_t *s, uint16_t count) {
  if (s->is_host) {
    #if CFG_TUH_ENABLED
    return usbh_edpt_xfer(s->hwid, s->ep_addr, count ? s->ep_buf : NULL, count);
  #endif
  } else {
    #if CFG_TUD_ENABLED
    if (s->ep_buf == NULL) {
      return usbd_edpt_xfer_fifo(s->hwid, s->ep_addr, &s->ff, count, false);
    } else {
      return usbd_edpt_xfer(s->hwid, s->ep_addr, count ? s->ep_buf : NULL, count, false);
    }
  #endif
  }
  return false;
}

static bool stream_release(tu_edpt_stream_t *s) {
  if (s->is_host) {
    #if CFG_TUH_ENABLED
    return usbh_edpt_release(s->hwid, s->ep_addr);
  #endif
  } else {
    #if CFG_TUD_ENABLED
    return usbd_edpt_release(s->hwid, s->ep_addr);
  #endif
  }
  return false;
}

//--------------------------------------------------------------------+
// Stream Write
//--------------------------------------------------------------------+
bool tu_edpt_stream_write_zlp_if_needed(tu_edpt_stream_t *s, uint32_t last_xferred_bytes) {
  // ZLP condition: no pending data, last transferred bytes is multiple of packet size
  TU_VERIFY(tu_fifo_empty(&s->ff) && last_xferred_bytes > 0 && (0 == (last_xferred_bytes & (s->mps - 1))));
  TU_VERIFY(stream_claim(s));
  TU_ASSERT(stream_xfer(s, 0));
  return true;
}

uint32_t tu_edpt_stream_write_xfer(tu_edpt_stream_t *s) {
  const uint16_t ff_count = tu_fifo_count(&s->ff);
  TU_VERIFY(ff_count > 0, 0); // skip if no data
  TU_VERIFY(stream_claim(s), 0);

  // Pull data from FIFO -> EP buf
  uint16_t count;
  if (s->ep_buf == NULL) {
    count = tu_fifo_count(&s->ff); // re-get count since fifo can be changed
  } else {
    count = tu_fifo_read_n(&s->ff, s->ep_buf, s->xfer_len);
  }

  if (count > 0) {
    TU_ASSERT(stream_xfer(s, count), 0);
    return count;
  } else {
    // Release endpoint since we don't make any transfer
    // Note: data is dropped if terminal is not connected
    stream_release(s);
    return 0;
  }
}

uint32_t tu_edpt_stream_write(tu_edpt_stream_t *s, const void *buffer, uint32_t bufsize) {
  TU_VERIFY(bufsize > 0);
  const uint16_t ret = tu_fifo_write_n(&s->ff, buffer, (uint16_t) tu_min32(bufsize, UINT16_MAX));

  // flush if fifo has more than packet size or
  // in rare case: fifo depth is configured too small (which never reach packet size)
  if ((tu_fifo_count(&s->ff) >= s->mps) || (tu_fifo_depth(&s->ff) < s->mps)) {
    tu_edpt_stream_write_xfer(s);
  }
  return ret;
}

uint32_t tu_edpt_stream_write_available(tu_edpt_stream_t *s) {
  return (uint32_t)tu_fifo_remaining(&s->ff);
}

//--------------------------------------------------------------------+
// Stream Read
//--------------------------------------------------------------------+
uint32_t tu_edpt_stream_read_xfer(tu_edpt_stream_t *s) {
  uint16_t available = tu_fifo_remaining(&s->ff);

  // Prepare for incoming data but only allow what we can store in the ring buffer.
  // TODO Actually we can still carry out the transfer, keeping count of received bytes
  // and slowly move it to the FIFO when read().
  // This pre-check reduces endpoint claiming
  TU_VERIFY(available >= s->mps);
  TU_VERIFY(stream_claim(s), 0);
  available = tu_fifo_remaining(&s->ff); // re-get available since fifo can be changed

  if (available >= s->mps) {
    // multiple of packet size limit by ep bufsize
    uint16_t count = (uint16_t) (available & ~(s->mps - 1));
    count = tu_min16(count, s->xfer_len);
    TU_ASSERT(stream_xfer(s, count), 0);
    return count;
  } else {
    // Release endpoint since we don't make any transfer
    stream_release(s);
    return 0;
  }
}

uint32_t tu_edpt_stream_read(tu_edpt_stream_t *s, void *buffer, uint32_t bufsize) {
  const uint32_t num_read = tu_fifo_read_n(&s->ff, buffer, (uint16_t) tu_min32(bufsize, UINT16_MAX));
  tu_edpt_stream_read_xfer(s);
  return num_read;
}

//--------------------------------------------------------------------+
// Debug
//--------------------------------------------------------------------+

#if CFG_TUSB_DEBUG
#include <ctype.h>

#if CFG_TUSB_DEBUG >= CFG_TUH_LOG_LEVEL || CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
char const* const tu_str_speed[] = {"Full", "Low", "High"};
char const* const tu_str_std_request[] = {
    "Get Status",
    "Clear Feature",
    "Reserved",
    "Set Feature",
    "Reserved",
    "Set Address",
    "Get Descriptor",
    "Set Descriptor",
    "Get Configuration",
    "Set Configuration",
    "Get Interface",
    "Set Interface",
    "Synch Frame"
};

char const* const tu_str_xfer_result[] = {
    "OK", "FAILED", "STALLED", "TIMEOUT", "ABORTED", "INVALID"
};
#endif

static void dump_str_line(uint8_t const* buf, uint16_t count) {
  tu_printf("  |");
  // each line is 16 bytes
  for (uint16_t i = 0; i < count; i++) {
    int ch = buf[i];
    tu_printf("%c", isprint(ch) ? ch : '.');
  }
  tu_printf("|\r\n");
}

/* Print out memory contents
 *  - buf   : buffer
 *  - count : number of item
 *  - indent: prefix spaces on every line
 */
void tu_print_mem(void const* buf, uint32_t count, uint8_t indent) {
  uint8_t const size = 1; // fixed 1 byte for now
  if (!buf || !count) {
    tu_printf("NULL\r\n");
    return;
  }

  uint8_t const* buf8 = (uint8_t const*) buf;
  char format[] = "%00X";
  format[2] += (uint8_t) (2 * size); // 1 byte = 2 hex digits
  const uint8_t item_per_line = 16 / size;

  for (unsigned int i = 0; i < count; i++) {
    unsigned int value = 0;

    if (i % item_per_line == 0) {
      // Print Ascii
      if (i != 0) {
        dump_str_line(buf8 - 16, 16);
      }
      for (uint8_t s = 0; s < indent; s++) {
        tu_printf(" ");
      }
      // print offset or absolute address
      tu_printf("%04X: ", 16 * i / item_per_line);
    }

    tu_memcpy_s(&value, sizeof(value), buf8, size);
    buf8 += size;

    tu_printf(" ");
    tu_printf(format, value);
  }

  // fill up last row to 16 for printing ascii
  const uint32_t remain = count % 16;
  uint8_t nback = (uint8_t) (remain ? remain : 16);
  if (remain > 0) {
    for (uint32_t i = 0; i < 16 - remain; i++) {
      tu_printf(" ");
      for (int j = 0; j < 2 * size; j++) {
        tu_printf(" ");
      }
    }
  }

  dump_str_line(buf8 - nback, nback);
}

#endif

#endif // host or device enabled
