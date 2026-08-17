#pragma once

#include <cstdint>

class SdCardFont {
 public:
  void clearCache() {}
  void releaseResidentCaches() {}
  int prewarm(const char*, uint8_t) { return 0; }
  void logStats(const char*) {}
  void resetStats() {}
};
