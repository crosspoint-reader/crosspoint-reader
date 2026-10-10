#pragma once

// Keep the SDK's OS, endpoint sizes, alignment, MSC and CDC configuration.
#include <tusb_config.h>

// USB Drive uses device mode; serial normally uses the separate USB Serial/JTAG peripheral.
#undef CFG_TUH_ENABLED
#define CFG_TUH_ENABLED 0
#undef CFG_TUSB_RHPORT1_MODE
#define CFG_TUSB_RHPORT1_MODE 0
#undef CFG_TUH_HUB
#define CFG_TUH_HUB 0
#undef CFG_TUH_CDC
#define CFG_TUH_CDC 0
#undef CFG_TUH_HID
#define CFG_TUH_HID 0
#undef CFG_TUH_MSC
#define CFG_TUH_MSC 0
#undef CFG_TUH_MIDI
#define CFG_TUH_MIDI 0

#undef CFG_TUD_HID
#define CFG_TUD_HID 0
#undef CFG_TUD_MIDI
#define CFG_TUD_MIDI 0
#undef CFG_TUD_AUDIO
#define CFG_TUD_AUDIO 0
#undef CFG_TUD_VIDEO
#define CFG_TUD_VIDEO 0
#undef CFG_TUD_DFU_RUNTIME
#define CFG_TUD_DFU_RUNTIME 0
#undef CFG_TUD_DFU
#define CFG_TUD_DFU 0
#undef CFG_TUD_VENDOR
#define CFG_TUD_VENDOR 0
#undef CFG_TUD_NCM
#define CFG_TUD_NCM 0
#undef CFG_TUD_CUSTOM_CLASS
#define CFG_TUD_CUSTOM_CLASS 0

#if !CFG_TUD_ENABLED || !CFG_TUD_MSC || !CFG_TUD_CDC
#error "CrossPoint USB Drive requires TinyUSB device MSC and CDC support"
#endif
