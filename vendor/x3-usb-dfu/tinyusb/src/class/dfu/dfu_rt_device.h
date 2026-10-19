/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Sylvain Munaut <tnt@246tNt.com>
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_DFU_RT_DEVICE_H_
#define TUSB_DFU_RT_DEVICE_H_

#include "dfu.h"

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// Application Callback API (weak is optional)
//--------------------------------------------------------------------+
// Invoked when a DFU_DETACH request is received and bitWillDetach is set
void tud_dfu_runtime_reboot_to_dfu_cb(void);

//--------------------------------------------------------------------+
// Internal Class Driver API
//--------------------------------------------------------------------+
void     dfu_rtd_init(void);
bool     dfu_rtd_deinit(void);
void     dfu_rtd_reset(uint8_t rhport);
uint16_t dfu_rtd_open(uint8_t rhport, tusb_desc_interface_t const * itf_desc, uint16_t max_len);
bool     dfu_rtd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request);

#ifdef __cplusplus
 }
#endif

#endif /* TUSB_DFU_RT_DEVICE_H_ */
