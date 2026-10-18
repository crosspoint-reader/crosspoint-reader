#include "NativeDfu.h"

#include <Arduino.h>
#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>

#include "UsbUpdater.h"
#include "class/dfu/dfu_device.h"
#include "device/usbd_pvt.h"

namespace {
// The native stack remains alive until deep sleep/reboot. Static buffers avoid
// a callback racing with workspace destruction; no allocation occurs in USB
// callbacks. Flash/display work runs on the existing DFU worker, not TinyUSB.
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
bool usbStarted = false;
struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};
enum class Job { None, Download, Manifest };
Job pending = Job::None;
alignas(4) uint8_t packet[usb_update::transferBlockSize];
uint16_t block = 0, length = 0;
uint32_t generation = 0;
bool processing = false;
bool accepting = false, installed = false, hostConfirmed = false;
bool completionStatusPending = false;
std::atomic<uint32_t> pollMs{8000};
uint8_t interfaceNumber = 0;

uint16_t descriptor(uint8_t* dst, uint8_t* itf) {
  interfaceNumber = *itf;
  const uint8_t desc[] = {TUD_DFU_DESCRIPTOR(*itf, 1, 0, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT, 1000,
                                             usb_update::transferBlockSize)};
  ++*itf;
  memcpy(dst, desc, sizeof(desc));
  return sizeof(desc);
}
void reset(uint8_t port) {
  Lock lock;
  ++generation;
  completionStatusPending = false;
  pending = Job::None;
  dfu_moded_reset(port);
}
bool control(uint8_t port, uint8_t stage, const tusb_control_request_t* request) {
  Lock lock;
  if (stage == CONTROL_STAGE_SETUP) completionStatusPending = false;
  if (request->wIndex != interfaceNumber) return false;
  // Idle queries remain available, but storage handoff rejects all mutations.
  if ((!accepting || installed) && request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
      request->bRequest != DFU_REQUEST_GETSTATUS && request->bRequest != DFU_REQUEST_GETSTATE)
    return false;
  if (processing && request->bRequest == DFU_REQUEST_DNLOAD && request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS)
    return false;
  const bool ok = dfu_moded_control_xfer_cb(port, stage, request);
  if (ok && stage == CONTROL_STAGE_SETUP && installed && request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
      request->bRequest == DFU_REQUEST_GETSTATUS) {
    completionStatusPending = true;
  }
  if (ok && completionStatusPending && stage == CONTROL_STAGE_ACK &&
      request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && request->bRequest == DFU_REQUEST_GETSTATUS) {
    hostConfirmed = true;
    completionStatusPending = false;
  }
  return ok;
}
const usbd_class_driver_t driver = {.name = "CrossPoint DFU",
                                    .init = dfu_moded_init,
                                    .deinit = dfu_moded_deinit,
                                    .reset = reset,
                                    .open = dfu_moded_open,
                                    .control_xfer_cb = control,
                                    .xfer_cb = nullptr,
                                    .xfer_isr = nullptr,
                                    .sof = nullptr};
uint8_t status(usb_update::Error error) {
  using usb_update::Error;
  switch (error) {
    case Error::None:
      return DFU_STATUS_OK;
    case Error::File:
      return DFU_STATUS_ERR_FILE;
    case Error::Address:
      return DFU_STATUS_ERR_ADDRESS;
    case Error::Write:
      return DFU_STATUS_ERR_WRITE;
    case Error::Verify:
      return DFU_STATUS_ERR_VERIFY;
    default:
      return DFU_STATUS_ERR_UNKNOWN;
  }
}
}  // namespace
extern "C" {
const usbd_class_driver_t* usbd_app_driver_get_cb(uint8_t* count) {
  *count = mutex ? 1 : 0;
  return mutex ? &driver : nullptr;
}
uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t state) { return state == DFU_MANIFEST ? 10000 : pollMs.load(); }
void tud_dfu_download_cb(uint8_t alt, uint16_t number, const uint8_t* data, uint16_t size) {
  // Called with the driver mutex held. Copy out before TinyUSB reuses its buffer.
  if (alt || pending != Job::None || size > sizeof(packet)) {
    tud_dfu_finish_flashing(DFU_STATUS_ERR_STALLEDPKT);
    return;
  }
  memcpy(packet, data, size);
  block = number;
  length = size;
  pending = Job::Download;
}
void tud_dfu_manifest_cb(uint8_t alt) {
  if (alt || pending != Job::None) {
    tud_dfu_finish_flashing(DFU_STATUS_ERR_STALLEDPKT);
    return;
  }
  pending = Job::Manifest;
}
void tud_dfu_abort_cb(uint8_t) {
  ++generation;
  pending = Job::None;
}
}
namespace native_dfu {
bool begin() {
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (!mutex) return false;
  if (tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_DFU_DESC_LEN(1), descriptor) != ESP_OK) return false;
  USB.VID(0x1209);
  USB.PID(0x0001);
  // Distinguishes the S3 board from the X3 transport in the browser validator.
  USB.firmwareVersion(0x0800);
  USB.productName("CrossPoint X4 Pro DFU");
  usbStarted = USB.begin();
  return usbStarted;
}
bool connected() { return usbStarted && tud_mounted() && !tud_suspended(); }
void disconnect() {
  if (usbStarted) tud_disconnect();
}
void reconnectForStorage() {
  if (usbStarted) tud_connect();
}
}  // namespace native_dfu
namespace usb_update {
void suspendCache() {}
bool resumeCache() { return true; }
Report run(Session& session, bool (*cancelled)(), uint32_t, uint32_t hostIdleMs, bool, void (*linkState)(bool)) {
  Report report;
  uint32_t epoch;
  {
    Lock lock;
    pending = Job::None;
    installed = hostConfirmed = completionStatusPending = false;
    accepting = true;
    epoch = generation;
    pollMs.store(8000);
  }
  uint32_t absentAt = 0, completeAt = 0;
  bool linked = false;
  while (!cancelled || !cancelled()) {
    const bool connected = native_dfu::connected();
    if (connected != linked) {
      linked = connected;
      if (linkState) linkState(linked);
    }
    if (connected) {
      absentAt = 0;
      report.mounted = 1;
    } else if (!absentAt)
      absentAt = millis();
    if (absentAt && uint32_t(millis() - absentAt) >= hostIdleMs) break;
    Job job = Job::None;
    uint32_t workEpoch;
    bool resetSession, confirmed;
    {
      Lock lock;
      workEpoch = generation;
      resetSession = epoch != workEpoch;
      confirmed = hostConfirmed;
      if (!resetSession) {
        job = pending;
        processing = job != Job::None;
      }
    }
    if (session.complete()) {
      if (!completeAt) completeAt = millis();
      if (confirmed || uint32_t(millis() - completeAt) >= 180000) break;
    } else if (resetSession) {
      session.abort();
      epoch = workEpoch;
      pollMs.store(8000);
      ++report.resets;
    } else if (job != Job::None) {
      // The DFU busy state keeps the mailbox stable until finish_flashing.
      const Error error = job == Job::Download ? session.download(block, packet, length) : session.manifest();
      Lock lock;
      processing = false;
      if (generation == workEpoch) {
        pending = Job::None;
        installed = session.complete();
        pollMs.store(session.pollTimeout(false));
        tud_dfu_finish_flashing(status(error));
      }
    }
    delay(1);
  }
  {
    Lock lock;
    accepting = false;
    pending = Job::None;
    report.hostConfirmed = hostConfirmed;
    dfu_moded_reset(0);
  }
  report.received = session.bytes();
  report.total = session.size();
  report.completed = session.complete();
  report.sessionError = static_cast<uint32_t>(session.error());
  if (!session.complete()) session.abort();
  if (linkState) linkState(false);
  return report;
}
}  // namespace usb_update
