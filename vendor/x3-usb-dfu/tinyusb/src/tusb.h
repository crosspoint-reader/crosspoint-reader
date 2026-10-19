/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_H_
#define TUSB_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "common/tusb_common.h"
#include "osal/osal.h"
#include "common/tusb_fifo.h"

//------------- TypeC -------------//
#if CFG_TUC_ENABLED
  #include "typec/usbc.h"
#endif

//------------- HOST -------------//
#if CFG_TUH_ENABLED
  #include "host/usbh.h"

  #if CFG_TUH_AUDIO
    #include "class/audio/audio_host.h"
  #endif

  #if CFG_TUH_HID
    #include "class/hid/hid_host.h"
  #endif

  #if CFG_TUH_MSC
    #include "class/msc/msc_host.h"
  #endif

  #if CFG_TUH_CDC
    #include "class/cdc/cdc_host.h"
  #endif

  #if CFG_TUH_MIDI
    #include "class/midi/midi_host.h"
  #endif

  #if CFG_TUH_MIDI2
    #include "class/midi/midi2_host.h"
  #endif

#else
  #ifndef tuh_int_handler
  #define tuh_int_handler(...)
  #endif
#endif

//------------- DEVICE -------------//
#if CFG_TUD_ENABLED
  #include "device/usbd.h"

  #if CFG_TUD_HID
    #include "class/hid/hid_device.h"
  #endif

  #if CFG_TUD_CDC
    #include "class/cdc/cdc_device.h"
  #endif

  #if CFG_TUD_MSC
    #include "class/msc/msc_device.h"
  #endif

  #if CFG_TUD_PRINTER
    #include "class/printer/printer_device.h"
  #endif

  #if CFG_TUD_MTP
    #include "class/mtp/mtp_device.h"
  #endif

  #if CFG_TUD_AUDIO
    #include "class/audio/audio_device.h"
  #endif

  #if CFG_TUD_VIDEO
    #include "class/video/video_device.h"
  #endif

  #if CFG_TUD_MIDI
    #include "class/midi/midi_device.h"
  #endif

  #if CFG_TUD_MIDI2
    #include "class/midi/midi2_device.h"
  #endif

  #if CFG_TUD_VENDOR
    #include "class/vendor/vendor_device.h"
  #endif

  #if CFG_TUD_USBTMC
    #include "class/usbtmc/usbtmc_device.h"
  #endif

  #if CFG_TUD_DFU_RUNTIME
    #include "class/dfu/dfu_rt_device.h"
  #endif

  #if CFG_TUD_DFU
    #include "class/dfu/dfu_device.h"
  #endif

  #if CFG_TUD_ECM_RNDIS || CFG_TUD_NCM
    #include "class/net/net_device.h"
  #endif

  #if CFG_TUD_BTH
    #include "class/bth/bth_device.h"
  #endif
#else
  #ifndef tud_int_handler
  #define tud_int_handler(...)
  #endif
#endif


//--------------------------------------------------------------------+
// Application API
//--------------------------------------------------------------------+
#if CFG_TUH_ENABLED || CFG_TUD_ENABLED

// Internal helper for backward compatible with tusb_init(void)
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init);

// Initialize roothub port with device/host role
// Note: when using with RTOS, this should be called after scheduler/kernel is started.
//       Since USB IRQ handler does use RTOS queue API.
// Note2: defined as macro for backward compatible with tusb_init(void), can be changed to function in the future.
#if defined(TUD_OPT_RHPORT) || defined(TUH_OPT_RHPORT)
  #define _tusb_init_arg0()        tusb_rhport_init(0, NULL)
#else
  #define _tusb_init_arg0()        TU_VERIFY_STATIC(false, "CFG_TUSB_RHPORT0_MODE/CFG_TUSB_RHPORT1_MODE must be defined")
#endif

#define _tusb_init_arg1(_rhport)             _tusb_init_arg0()
#define _tusb_init_arg2(_rhport, _rh_init)   tusb_rhport_init(_rhport, _rh_init)
#define tusb_init(...)                       TU_FUNC_OPTIONAL_ARG(_tusb_init, __VA_ARGS__)

// Check if stack is initialized
bool tusb_inited(void);

// Called to handle usb interrupt/event. tusb_init(rhport, role) must be called before
void tusb_int_handler(uint8_t rhport, bool in_isr);

// Deinit usb stack on roothub port
bool tusb_deinit(uint8_t rhport);

#else

#define tusb_init(...)  (false)
#define tusb_int_handler(...)  do {}while(0)
#define tusb_inited()  (false)
#define tusb_deinit(...) (false)

#endif

#ifdef __cplusplus
 }
#endif

#endif /* TUSB_H_ */
