#include <Arduino.h>

#include "UsbIdentity.h"
#include "UsbUpdater.h"
#include "tusb.h"
namespace usb_update {
Session* activeSession = nullptr;
}
#ifdef USB_UPDATER_TEST_SINK
static constexpr uint16_t version = 0x0601;
#else
static constexpr uint16_t version = 0x0700;
#endif
static const tusb_desc_device_t device = {18, TUSB_DESC_DEVICE, 0x0200, 0, 0, 0, 64, 0x1209, 1, version, 1, 2, 0, 1};
static const uint8_t config[] = {TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1), 0, 100),
                                 TUD_DFU_DESCRIPTOR(0, 1, 0, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT,
                                                    1000, usb_update::transferBlockSize)};
static_assert(CFG_TUD_DFU_XFER_BUFSIZE == usb_update::transferBlockSize);
static uint8_t status(usb_update::Error e) {
  using usb_update::Error;
  switch (e) {
    case Error::None:
      return DFU_STATUS_OK;
    case Error::Address:
      return DFU_STATUS_ERR_ADDRESS;
    case Error::File:
      return DFU_STATUS_ERR_FILE;
    case Error::Write:
      return DFU_STATUS_ERR_WRITE;
    case Error::Verify:
      return DFU_STATUS_ERR_VERIFY;
    default:
      return DFU_STATUS_ERR_UNKNOWN;
  }
}
extern "C" {
const uint8_t* tud_descriptor_device_cb() { return reinterpret_cast<const uint8_t*>(&device); }
const uint8_t* tud_descriptor_configuration_cb(uint8_t index) { return index == 0 ? config : nullptr; }
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t language) {
  return usb_update::stringDescriptor(index, language);
}
uint32_t tusb_time_millis_api() { return millis(); }
uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t state) {
  return usb_update::activeSession ? usb_update::activeSession->pollTimeout(state == DFU_MANIFEST) : 1000;
}
void tud_dfu_download_cb(uint8_t alt, uint16_t block, const uint8_t* data, uint16_t length) {
  auto* s = usb_update::activeSession;
  tud_dfu_finish_flashing(status(s && alt == 0 ? s->download(block, data, length) : usb_update::Error::Unavailable));
}
void tud_dfu_manifest_cb(uint8_t alt) {
  auto* s = usb_update::activeSession;
  tud_dfu_finish_flashing(status(s && alt == 0 ? s->manifest() : usb_update::Error::Unavailable));
}
void tud_dfu_abort_cb(uint8_t) {
  if (usb_update::activeSession) usb_update::activeSession->abort();
}
void tud_dfu_detach_cb() {}
uint16_t tud_dfu_upload_cb(uint8_t, uint16_t, uint8_t*, uint16_t) { return 0; }
}
