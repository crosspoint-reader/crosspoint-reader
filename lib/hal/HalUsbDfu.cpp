#include "HalUsbDfu.h"

#include <sdkconfig.h>

#include <atomic>

#include "UsbDfuPolicy.h"

#if CONFIG_IDF_TARGET_ESP32C3
#include <Arduino.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_rom_sys.h>
#include <soc/gpio_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "UsbIdentity.h"

namespace {
bool fallbackEnabled = false;
bool armed = false;
std::atomic<bool> usbReady{false}, usbConfigured{false};
void (*stopHook)() = nullptr;
volatile bool hostRequested = false;
constexpr uint32_t USB_PINS = BIT(18) | BIT(19);
constexpr uint32_t USB_PULLS = USB_SERIAL_JTAG_PAD_PULL_OVERRIDE | USB_SERIAL_JTAG_DP_PULLUP |
                               USB_SERIAL_JTAG_DM_PULLUP | USB_SERIAL_JTAG_DP_PULLDOWN | USB_SERIAL_JTAG_DM_PULLDOWN |
                               USB_SERIAL_JTAG_PULLUP_VALUE;
uint32_t disconnectedConfig = 0;

void IRAM_ATTR onDpFalling() {
  if (hostRequested || (REG_READ(GPIO_IN_REG) & USB_PINS)) return;
  // Full-speed packet EOP is much shorter than a host reset. No scheduler,
  // allocation, logging or flash access is allowed in this wake detector.
  esp_rom_delay_us(12);
  if ((REG_READ(GPIO_IN_REG) & USB_PINS) == 0) hostRequested = true;
}
}  // namespace

void HalUsbDfu::begin(bool isX3, bool isX4) {
  fallbackEnabled = usb_dfu::needsFallback(isX3, isX4, esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG),
                                           esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE),
                                           esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE));
  if (!fallbackEnabled) return;
  usb_update::identityIsX3 = isX3;
#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif
  disconnectedConfig = (REG_READ(USB_SERIAL_JTAG_CONF0_REG) & ~(USB_PULLS | USB_SERIAL_JTAG_USB_PAD_ENABLE)) |
                       USB_SERIAL_JTAG_PAD_PULL_OVERRIDE;
  LOG_INF("DFU", "Native USB download disabled by eFuse; enabling automatic software DFU");
  arm();
}

bool HalUsbDfu::enabled() { return fallbackEnabled; }
void HalUsbDfu::arm() {
  if (!fallbackEnabled || armed) return;
  hostRequested = false;
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, disconnectedConfig);
  pinMode(18, INPUT);
  pinMode(19, INPUT);
  attachInterrupt(19, onDpFalling, FALLING);
  armed = true;
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, disconnectedConfig | USB_SERIAL_JTAG_DP_PULLUP);
}
void HalUsbDfu::stop() {
  if (!fallbackEnabled) return;
  if (stopHook) stopHook();
  usbReady.store(false);
  usbConfigured.store(false);
  if (armed) detachInterrupt(19);
  armed = false;
  hostRequested = false;
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, disconnectedConfig);
}
bool HalUsbDfu::takeHostRequest() {
  if (!armed || !hostRequested) return false;
  stop();
  return true;
}
void HalUsbDfu::setStopHook(void (*hook)()) { stopHook = hook; }
void HalUsbDfu::setReady(bool value) { usbReady.store(value); }
bool HalUsbDfu::ready() { return usbReady.load(); }
void HalUsbDfu::setConfigured(bool value) { usbConfigured.store(value); }
bool HalUsbDfu::configured() { return usbConfigured.load(); }
#elif CROSSPOINT_NATIVE_DFU
#include <Arduino.h>
#include <Logging.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include "NativeDfu.h"

namespace {
bool fallbackEnabled = false, armed = false;
std::atomic<bool> usbReady{false}, usbConfigured{false};
void (*stopHook)() = nullptr;
}  // namespace
void HalUsbDfu::begin(bool, bool) {
  if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_OTG)) {
    LOG_ERR("DFU", "USB OTG is disabled by eFuse; native DFU unavailable");
    return;
  }
#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif
  fallbackEnabled = native_dfu::begin();
  armed = fallbackEnabled;
  if (!fallbackEnabled) LOG_ERR("DFU", "Native USB initialization failed");
}
bool HalUsbDfu::enabled() { return fallbackEnabled; }
void HalUsbDfu::arm() { armed = fallbackEnabled; }
bool HalUsbDfu::takeHostRequest() {
  if (!armed || !native_dfu::connected()) return false;
  armed = false;
  return true;
}
void HalUsbDfu::stop() {
  armed = false;
  if (stopHook) stopHook();
  usbReady.store(false);
  usbConfigured.store(false);
  native_dfu::disconnect();
}
void HalUsbDfu::setStopHook(void (*hook)()) { stopHook = hook; }
void HalUsbDfu::setReady(bool value) { usbReady.store(value); }
bool HalUsbDfu::ready() { return usbReady.load(); }
void HalUsbDfu::setConfigured(bool value) { usbConfigured.store(value); }
bool HalUsbDfu::configured() { return usbConfigured.load(); }
#else
void HalUsbDfu::setStopHook(void (*)()) {}
void HalUsbDfu::setReady(bool) {}
void HalUsbDfu::setConfigured(bool) {}
bool HalUsbDfu::configured() { return false; }
bool HalUsbDfu::ready() { return false; }
void HalUsbDfu::begin(bool, bool) {}
bool HalUsbDfu::enabled() { return false; }
bool HalUsbDfu::takeHostRequest() { return false; }
void HalUsbDfu::arm() {}
void HalUsbDfu::stop() {}
#endif
