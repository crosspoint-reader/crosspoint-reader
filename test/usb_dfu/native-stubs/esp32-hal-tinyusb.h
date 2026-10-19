#pragma once
#include <atomic>
#include <cstdint>
constexpr int ESP_OK = 0, USB_INTERFACE_CUSTOM = 8;
constexpr int DFU_ATTR_CAN_DOWNLOAD = 1, DFU_ATTR_MANIFESTATION_TOLERANT = 4;
constexpr int TUSB_REQ_TYPE_CLASS = 1, CONTROL_STAGE_SETUP = 0, CONTROL_STAGE_ACK = 2;
constexpr int DFU_REQUEST_DNLOAD = 1, DFU_REQUEST_GETSTATUS = 3, DFU_REQUEST_GETSTATE = 5;
constexpr int DFU_MANIFEST = 7;
constexpr int DFU_STATUS_OK = 0, DFU_STATUS_ERR_FILE = 2, DFU_STATUS_ERR_WRITE = 3, DFU_STATUS_ERR_VERIFY = 7,
              DFU_STATUS_ERR_ADDRESS = 8, DFU_STATUS_ERR_UNKNOWN = 14, DFU_STATUS_ERR_STALLEDPKT = 15;
#define TUD_DFU_DESC_LEN(n) 18
#define TUD_DFU_DESCRIPTOR(itf, n, s, a, t, b) itf, n, s, a, 0, 0
struct tusb_control_request_t {
  struct {
    uint8_t type;
  } bmRequestType_bit;
  uint8_t bRequest;
  uint16_t wIndex;
};
struct tusb_desc_interface_t {};
using xfer_result_t = int;
inline std::atomic<bool> mounted{true};
inline bool tud_mounted() { return mounted.load(); }
inline bool tud_suspended() { return false; }
inline void tud_disconnect() { mounted = false; }
inline void tud_connect() { mounted = true; }
inline int tinyusb_enable_interface(int, int, uint16_t (*cb)(uint8_t*, uint8_t*)) {
  uint8_t data[32], itf = 1;
  cb(data, &itf);
  return ESP_OK;
}
