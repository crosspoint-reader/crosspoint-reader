#pragma once
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64
// TinyUSB's generic non-EP0 path requires storage even though this DCD refuses
// to open non-control endpoints. Two slots also keep GCC's bounds checks clean.
#define TUP_DCD_ENDPOINT_MAX 2
#define CFG_TUD_DFU 1
#define CFG_TUD_DFU_XFER_BUFSIZE 4096
#define CFG_TUSB_DEBUG 0
