#pragma once
#include "UsbDfuUpdate.h"

namespace usb_dfu {
struct Snapshot {
  Stage stage = Stage::Waiting;
  uint32_t bytes = 0, total = 0, elapsedMs = 0;
  bool running = false, installed = false, screenRequested = false;
};
bool startBackground();
void stopBackground();
Snapshot snapshot();
bool takeScreenRequest();
void allowTransfer();
void releaseScreen();
void rejectTransfer();
void acknowledgeStopped();
}  // namespace usb_dfu
