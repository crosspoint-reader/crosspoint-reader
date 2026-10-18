// Turn the upstream array declaration into a pointer-to-array declaration.
// Every upstream expression still sees an array of the same size, including
// sizeof(_transfer_buf); TinyUSB's source and DFU packet handling stay intact.
#define _transfer_buf (*su_dfu_transfer_storage)
#include "../tinyusb/src/class/dfu/dfu_device.c"
#undef _transfer_buf
#include "DfuBuffer.h"
#include "hot_tinyusb.h"

bool su_dfu_bind_buffer(uint8_t* buffer, size_t size) {
  if (!buffer) {
    if (size) return false;
    su_dfu_transfer_storage = NULL;
    return true;
  }
  if ((uintptr_t)buffer % 4 || size < CFG_TUD_DFU_XFER_BUFSIZE) return false;
  su_dfu_transfer_storage = (uint8_t (*)[CFG_TUD_DFU_XFER_BUFSIZE])buffer;
  return true;
}
