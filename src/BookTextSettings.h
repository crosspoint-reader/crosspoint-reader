#pragma once

#include <cstdint>
#include <string>

#include "ReaderTextSettings.h"

class BookTextSettings {
 public:
  void load(const std::string& cachePath);
  ReaderTextSettings resolve() const;
  bool update(const ReaderTextSettings& edited);
  bool reset();
  bool hasOverrides() const { return overrideMask != 0; }

 private:
  std::string cachePath;
  ReaderTextSettings overrides;
  uint16_t overrideMask = 0;

  bool save(const ReaderTextSettings& values, uint16_t mask) const;
};
