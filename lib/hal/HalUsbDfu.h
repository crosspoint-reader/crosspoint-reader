#pragma once

// Read-only eFuse gate, host-reset detection, and software USB link state.
namespace HalUsbDfu {
void begin(bool isX3, bool isX4 = false);
bool enabled();
bool takeHostRequest();
void arm();
void stop();
void setStopHook(void (*hook)());
void setReady(bool ready);
bool ready();
void setConfigured(bool configured);
bool configured();
}  // namespace HalUsbDfu
