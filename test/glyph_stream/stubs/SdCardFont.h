#pragma once

#include <cstdint>

class SdCardFont {
 public:
  void clearCache() {}
  void releaseResidentCaches() {}
  int prewarm(const char*, uint8_t) { return 0; }
  uint8_t resolveStyle(uint8_t style) const { return style & 0x03; }
  void logStats(const char*) {}
  void resetStats() {}
};
