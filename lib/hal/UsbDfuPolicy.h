#pragma once

namespace usb_dfu {
// DIS_USB_JTAG only disables debugging, not USB serial/download.
constexpr bool needsFallback(bool isX3, bool isX4, bool controllerDisabled, bool usbDownloadDisabled,
                             bool downloadDisabled) {
  return (isX3 || isX4) && (controllerDisabled || usbDownloadDisabled || downloadDisabled);
}
}  // namespace usb_dfu
