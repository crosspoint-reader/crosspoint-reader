/*
 * SPDX-FileCopyrightText: Copyright (c) 2022, Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_PRIVATE_H
#define TUSB_PRIVATE_H

// Internal Helper used by Host and Device Stack

#ifdef __cplusplus
 extern "C" {
#endif

typedef void (*tusb_defer_func_t)(uintptr_t param);

 //--------------------------------------------------------------------+
 // Configuration
 //--------------------------------------------------------------------+

#define TUP_USBIP_CONTROLLER_NUM 2
extern tusb_role_t _tusb_rhport_role[TUP_USBIP_CONTROLLER_NUM];

//--------------------------------------------------------------------+
// Endpoint
//--------------------------------------------------------------------+

// Endpoint state bits — manipulate the bare uint8_t with these masks.
#define TU_EDPT_STATE_BUSY    0x01u
#define TU_EDPT_STATE_STALLED 0x02u
#define TU_EDPT_STATE_CLAIMED 0x04u
#define TU_EDPT_STATE_RX_PENDING 0x08u // OUT xfer completed, ep buffer not yet consumed by the class (#1292)

typedef struct {
  uint8_t  hwid;    // device: rhport, host: daddr
  bool     is_host; // 1: host, 0: device
  uint8_t ep_addr;
  // 1 byte padding

  uint16_t mps;
  uint16_t xfer_len;
  uint8_t  *ep_buf; // set to NULL to use xfer_fifo when CFG_TUD_EDPT_DEDICATED_HWFIFO = 1
  tu_fifo_t ff;

  // mutex: read if rx, otherwise write
  OSAL_MUTEX_DEF(ff_mutexdef);
}tu_edpt_stream_t;

//--------------------------------------------------------------------+
// Endpoint
//--------------------------------------------------------------------+

// Check if endpoint descriptor is valid per USB specs if debug is enabled
#if CFG_TUSB_DEBUG
bool tu_edpt_validate(const tusb_desc_endpoint_t *desc_ep, tusb_speed_t speed);
#else
TU_ATTR_ALWAYS_INLINE static inline bool tu_edpt_validate(const tusb_desc_endpoint_t *desc_ep, tusb_speed_t speed) {
  (void)speed;
  return tu_edpt_packet_size(desc_ep) > 0;
}
#endif

// Bind drivers to all interfaces and endpoints in the provided configuration descriptor
bool tu_bind_driver_to_ep_itf(uint8_t driver_id, uint8_t ep2drv[][2], uint8_t itf2drv[], uint8_t itf_max,
                              uint8_t ep_max, const uint8_t *p_desc, uint16_t desc_len);

// Claim an endpoint with provided mutex
bool tu_edpt_claim(volatile uint8_t* ep_state, osal_mutex_t mutex);

// Release an endpoint with provided mutex
bool tu_edpt_release(volatile uint8_t* ep_state, osal_mutex_t mutex);

//--------------------------------------------------------------------+
// Endpoint Stream
//--------------------------------------------------------------------+

// Init an endpoint stream
bool tu_edpt_stream_init(tu_edpt_stream_t *s, bool is_host, bool is_tx, bool overwritable, void *ff_buf,
                         uint16_t ff_bufsize, uint8_t *ep_buf);

// Deinit an endpoint stream
TU_ATTR_ALWAYS_INLINE static inline void tu_edpt_stream_deinit(tu_edpt_stream_t *s) {
  (void)s;
#if OSAL_MUTEX_REQUIRED
  if (s->ff.mutex_wr) {
    osal_mutex_delete(s->ff.mutex_wr);
  }
  if (s->ff.mutex_rd) {
    osal_mutex_delete(s->ff.mutex_rd);
  }
#endif
}

// Open an endpoint stream
TU_ATTR_ALWAYS_INLINE static inline void tu_edpt_stream_open(tu_edpt_stream_t *s, uint8_t hwid,
                                                             const tusb_desc_endpoint_t *desc_ep, uint16_t xfer_len) {
  s->hwid    = hwid;
  s->ep_addr = desc_ep->bEndpointAddress;
  s->mps = tu_edpt_packet_size(desc_ep);
  s->xfer_len = xfer_len;
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_edpt_stream_is_opened(const tu_edpt_stream_t *s) {
  return s->ep_addr != 0;
}

TU_ATTR_ALWAYS_INLINE static inline void tu_edpt_stream_close(tu_edpt_stream_t* s) {
  s->ep_addr = 0;
}

TU_ATTR_ALWAYS_INLINE static inline void tu_edpt_stream_clear(tu_edpt_stream_t *s) {
  tu_fifo_clear(&s->ff);
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_edpt_stream_empty(tu_edpt_stream_t *s) {
  return tu_fifo_empty(&s->ff);
}

//--------------------------------------------------------------------+
// Stream Write
//--------------------------------------------------------------------+

// Write to stream
uint32_t tu_edpt_stream_write(tu_edpt_stream_t *s, const void *buffer, uint32_t bufsize);

// Start an usb transfer if endpoint is not busy. Return number of queued bytes
uint32_t tu_edpt_stream_write_xfer(tu_edpt_stream_t *s);

// Start an zero-length packet if needed
bool tu_edpt_stream_write_zlp_if_needed(tu_edpt_stream_t *s, uint32_t last_xferred_bytes);

// Get the number of bytes available for writing to FIFO
// Note: if no fifo, return endpoint size if not busy, 0 otherwise
uint32_t tu_edpt_stream_write_available(tu_edpt_stream_t *s);

//--------------------------------------------------------------------+
// Stream Read
//--------------------------------------------------------------------+

// Read from stream
uint32_t tu_edpt_stream_read(tu_edpt_stream_t *s, void *buffer, uint32_t bufsize);

// Start an usb transfer if endpoint is not busy
uint32_t tu_edpt_stream_read_xfer(tu_edpt_stream_t *s);

#if CFG_TUD_ENABLED
// Release an OUT endpoint's RX_PENDING hold once its buffer is consumed, allowing it to be claimed
void usbd_edpt_rx_consume(uint8_t rhport, uint8_t ep_addr);
#else
  #define usbd_edpt_rx_consume(_rhport, _ep_addr)
#endif

// Complete read transfer by writing EP -> FIFO. Must be called in the transfer complete callback
TU_ATTR_ALWAYS_INLINE static inline
void tu_edpt_stream_read_xfer_complete(tu_edpt_stream_t* s, uint32_t xferred_bytes) {
  if (s->ep_buf != NULL) {
    tu_fifo_write_n(&s->ff, s->ep_buf, (uint16_t)xferred_bytes);
  }
  if (!s->is_host) {
    usbd_edpt_rx_consume(s->hwid, s->ep_addr);
  }
}

// Complete read transfer with provided buffer
TU_ATTR_ALWAYS_INLINE static inline
void tu_edpt_stream_read_xfer_complete_with_buf(tu_edpt_stream_t *s, const void *buf, uint32_t xferred_bytes) {
  tu_fifo_write_n(&s->ff, buf, (uint16_t)xferred_bytes);
}

// Get the number of bytes available for reading
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_edpt_stream_read_available(const tu_edpt_stream_t *s) {
  return (uint32_t) tu_fifo_count(&s->ff);
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_edpt_stream_peek(tu_edpt_stream_t *s, uint8_t *ch) {
  return tu_fifo_peek(&s->ff, ch);
}

#ifdef __cplusplus
 }
#endif

#endif
