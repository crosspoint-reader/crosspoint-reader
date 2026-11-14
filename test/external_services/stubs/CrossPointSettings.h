#pragma once

#include <cstdint>

class CrossPointSettings {
 public:
  static CrossPointSettings& getInstance() {
    static CrossPointSettings settings;
    return settings;
  }

  uint8_t externalServicesEnabled = 0;
};

#define SETTINGS CrossPointSettings::getInstance()
