#pragma once

// Arduino fs::FS facade over HalStorage/HalFile (SdFat underneath).
//
// Exists for third-party libraries that take an fs::FS& (ESP32-audioI2S's
// connecttoFS in particular). CrossPoint's own code must keep using
// HalStorage/HalFile directly — this adapter is a bridge, not a second
// storage API. Every operation routes through HalFile, so the HalStorage
// mutex serializes it against the rest of the firmware (SdFat is not
// thread-safe; see HalStorage.h). Read-oriented: writes and directory
// iteration are stubbed to the minimum fs::FS requires.

#include <FS.h>

// Returns the process-wide fs::FS view of the SD card. Storage.begin() must
// have succeeded before any use.
fs::FS& sdFatFS();
