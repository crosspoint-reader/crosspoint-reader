#pragma once

namespace UsbMscPrompt {

inline bool shouldShowOnUsbConnect(const bool promptEnabled, const bool usbConnected, const bool usbConnectedLast,
                                   const bool hostSupportsUsbSerial, const bool sessionIdle) {
  return promptEnabled && usbConnected && !usbConnectedLast && hostSupportsUsbSerial && sessionIdle;
}

}  // namespace UsbMscPrompt
