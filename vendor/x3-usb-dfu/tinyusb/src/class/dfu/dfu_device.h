/*
 * SPDX-FileCopyrightText: Copyright (c) 2021 XMOS LIMITED
 * SPDX-FileCopyrightText: Copyright (c) 2021 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_DFU_DEVICE_H_
#define TUSB_DFU_DEVICE_H_

#include "dfu.h"

#ifdef __cplusplus
  extern "C" {
#endif

//--------------------------------------------------------------------+
// Class Driver Default Configure & Validation
//--------------------------------------------------------------------+

#if !defined(CFG_TUD_DFU_XFER_BUFSIZE)
  #error "CFG_TUD_DFU_XFER_BUFSIZE must be defined, it has to be set to the buffer size used in TUD_DFU_DESCRIPTOR"
#endif

//--------------------------------------------------------------------+
// Application API
//--------------------------------------------------------------------+

// Must be called when the application is done with flashing started by
// tud_dfu_download_cb() and tud_dfu_manifest_cb().
// status is DFU_STATUS_OK if successful, any other error status will cause state to enter dfuError
void tud_dfu_finish_flashing(uint8_t status);

//--------------------------------------------------------------------+
// Application Callback API (weak is optional)
//--------------------------------------------------------------------+

// Note: alt is used as the partition number, in order to support multiple partitions like FLASH, EEPROM, etc.

// Invoked right before tud_dfu_download_cb() (state=DFU_DNBUSY) or tud_dfu_manifest_cb() (state=DFU_MANIFEST)
// Application return timeout in milliseconds (bwPollTimeout) for the next download/manifest operation.
// During this period, USB host won't try to communicate with us.
uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state);

// Invoked when received DFU_DNLOAD (wLength>0) following by DFU_GETSTATUS (state=DFU_DNBUSY) requests
// This callback could be returned before flashing op is complete (async).
// Once finished flashing, application must call tud_dfu_finish_flashing()
void tud_dfu_download_cb (uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length);

// Invoked when download process is complete, received DFU_DNLOAD (wLength=0) following by DFU_GETSTATUS (state=Manifest)
// Application can do checksum, or actual flashing if buffered entire image previously.
// Once finished flashing, application must call tud_dfu_finish_flashing()
void tud_dfu_manifest_cb(uint8_t alt);

// Invoked when received DFU_UPLOAD request
// Application must populate data with up to length bytes and
// Return the number of written bytes
uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t* data, uint16_t length);

// Invoked when a DFU_DETACH request is received
void tud_dfu_detach_cb(void);

// Invoked when the Host has terminated a download or upload transfer
void tud_dfu_abort_cb(uint8_t alt);

//--------------------------------------------------------------------+
// Internal Class Driver API
//--------------------------------------------------------------------+
void     dfu_moded_init(void);
bool     dfu_moded_deinit(void);
void     dfu_moded_reset(uint8_t rhport);
uint16_t dfu_moded_open(uint8_t rhport, tusb_desc_interface_t const * itf_desc, uint16_t max_len);
bool     dfu_moded_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request);


#ifdef __cplusplus
 }
#endif

#endif /* TUSB_DFU_MODE_DEVICE_H_ */
