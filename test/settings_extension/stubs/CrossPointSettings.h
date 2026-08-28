#pragma once

#include <cstdint>

// Minimal stand-in for src/CrossPointSettings.h: the real header pulls in
// ArduinoJson and PersistableStore (SD-card/FreeRTOS backed), neither
// available in a host-test build. SettingsTypes.h only needs the type to
// exist (for the `uint8_t CrossPointSettings::*` member-pointer field and
// the SETTINGS macro used by SettingInfo::String()) -- this test exercises
// the Dynamic*/Extension paths, which never dereference it.
class CrossPointSettings {
 public:
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
  uint8_t placeholderField = 0;
};

#define SETTINGS CrossPointSettings::getInstance()
