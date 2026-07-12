#pragma once

// High-level dual-OS slot API layered on ota_boot::switchTo (OtaBootSwitch.h).
// Shared by CrossPoint and the vendored Biscuit firmware (biscuit/): each OS
// lives in one of the two OTA app slots and uses this to inspect and boot the
// other one.
//
// Identity caveat: the prebuilt Arduino core bakes a generic project name into
// every app descriptor, so the passive slot cannot be *named* — callers get
// blank/corrupt detection plus a best-effort version string only.

namespace boot_switch {

struct PassiveSlotInfo {
  char label[8];     // partition label, e.g. "app1"
  char version[33];  // esp_app_desc version string, "" if descriptor absent
};

// Fills `out` with info about the passive (non-running) OTA app slot.
// Returns false when there is no passive slot or it does not hold a
// plausible app image (blank/corrupt). Never runs esp_image_verify — that
// rejects X4-patched images (see OtaBootSwitch.h).
bool peekPassiveSlot(PassiveSlotInfo& out);

// Repoint otadata at the passive slot via ota_boot::switchTo. Refuses if the
// slot does not hold a plausible image. Returns false on failure; on success
// the caller is responsible for ESP.restart() (same contract as
// firmware_flash::flashFromSdPath).
bool swapToPassive();

}  // namespace boot_switch
