/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */
#ifndef TUSB_USBD_PVT_H_
#define TUSB_USBD_PVT_H_

#include "osal/osal.h"
#include "common/tusb_fifo.h"
#include "common/tusb_private.h"

#ifdef __cplusplus
 extern "C" {
#endif

#define TU_LOG_USBD(...)   TU_LOG(CFG_TUD_LOG_LEVEL, __VA_ARGS__)

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

typedef enum {
  SOF_CONSUMER_USER = 0,
  SOF_CONSUMER_AUDIO,
} sof_consumer_t;

//--------------------------------------------------------------------+
// Class Driver API
//--------------------------------------------------------------------+

typedef struct {
  char const* name;
  void     (* init             ) (void);
  bool     (* deinit           ) (void);
  void     (* reset            ) (uint8_t rhport);
  uint16_t (* open             ) (uint8_t rhport, tusb_desc_interface_t const * desc_intf, uint16_t max_len);
  bool     (* control_xfer_cb  ) (uint8_t rhport, uint8_t stage, tusb_control_request_t const * request);
  bool     (* xfer_cb          ) (uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
  bool     (* xfer_isr         ) (uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes); // optional, return false to defer to xfer_cb()
  void     (* sof              ) (uint8_t rhport, uint32_t frame_count); // optional
} usbd_class_driver_t;

// Invoked when initializing device stack to get additional class drivers.
// Can be implemented by application to extend/overwrite class driver support.
// Note: The drivers array must be accessible at all time when stack is active
usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count);

typedef bool (*usbd_control_xfer_cb_t)(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request);

void usbd_int_set(bool enabled);
void usbd_spin_lock(bool in_isr);
void usbd_spin_unlock(bool in_isr);

uint8_t* usbd_get_ctrl_buf(void);

//--------------------------------------------------------------------+
// USBD Endpoint API
// Note: rhport should be 0 since device stack only support 1 rhport for now
//--------------------------------------------------------------------+

// Open an endpoint
bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const * desc_ep);

// Close an endpoint
void usbd_edpt_close(uint8_t rhport, uint8_t ep_addr);

// Submit a usb transfer
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t * buffer, uint16_t total_bytes, bool is_isr);

// Submit a usb ISO transfer by use of a FIFO (ring buffer) - all bytes in FIFO get transmitted
bool usbd_edpt_xfer_fifo(uint8_t rhport, uint8_t ep_addr, tu_fifo_t * ff, uint16_t total_bytes, bool is_isr);

// Claim an endpoint before submitting a transfer.
// If caller does not make any transfer, it must release endpoint for others.
// From an OUT xfer_cb, call usbd_edpt_rx_consume() (common/tusb_private.h) before claiming, or re-arm with
// usbd_edpt_xfer() directly: the claim is refused until the buffer is consumed.
bool usbd_edpt_claim(uint8_t rhport, uint8_t ep_addr);

// Release claimed endpoint without submitting a transfer
bool usbd_edpt_release(uint8_t rhport, uint8_t ep_addr);

// Check if endpoint is busy transferring
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr);

// Stall endpoint
void usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr);

// Clear stalled endpoint
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr);

// Check if endpoint is stalled
bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep_addr);

// Abort an in-flight transfer on a non-control endpoint and release its claim, so the endpoint
// can be armed again
void usbd_edpt_abort(uint8_t rhport, uint8_t ep_addr);

// Allocate packet buffer used by ISO endpoints
bool usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size);

// Configure and enable an ISO endpoint according to descriptor
bool usbd_edpt_iso_activate(uint8_t rhport,  tusb_desc_endpoint_t const * p_endpoint_desc);

// Check if endpoint is ready (not busy and not stalled)
TU_ATTR_ALWAYS_INLINE static inline
bool usbd_edpt_ready(uint8_t rhport, uint8_t ep_addr) {
  const bool is_busy = usbd_edpt_busy(rhport, ep_addr);
  const bool is_stalled = usbd_edpt_stalled(rhport, ep_addr);
  return !is_busy && !is_stalled;
}

// Enable SOF interrupt
void usbd_sof_enable(uint8_t rhport, sof_consumer_t consumer, bool en);

bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const* p_desc, uint8_t ep_count, uint8_t xfer_type, uint8_t* ep_out, uint8_t* ep_in);
void usbd_defer_func(osal_task_func_t func, void *param, bool in_isr);

// Call func(param) in the usbd task once its event queue is found empty, i.e. after every event
// queued before this call. Nothing is queued, so it neither blocks nor is lost on a full queue.
// usbd task only. One call is held at a time: false if a different one is pending.
bool usbd_defer_func_after_queue(osal_task_func_t func, void *param);

#ifdef __cplusplus
 }
#endif

#endif
