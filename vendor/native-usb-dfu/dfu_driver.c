// Register only the DFU class with Arduino's native TinyUSB stack. Never
// compile the C3 software DCD or a second copy of the USB device core here.
#include "tusb.h"
#undef CFG_TUD_DFU
#define CFG_TUD_DFU 1
#undef CFG_TUD_DFU_XFER_BUFSIZE
#define CFG_TUD_DFU_XFER_BUFSIZE 4096
#include "../x3-usb-dfu/tinyusb/src/class/dfu/dfu_device.c"
