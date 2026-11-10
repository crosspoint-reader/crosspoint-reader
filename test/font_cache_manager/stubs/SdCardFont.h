#pragma once

#include <cstdint>
#include <cstdio>

class SdCardFont {
 public:
  struct PrewarmCall {
    char text[32] = {};
    uint8_t styleMask = 0;
    bool accumulate = true;
  };

  void clearCache() {}
  void releaseResidentCaches() {}
  int prewarm(const char* text, uint8_t styleMask, bool, bool, bool accumulate) {
    auto& call = prewarmCalls[prewarmCallCount++];
    std::snprintf(call.text, sizeof(call.text), "%s", text);
    call.styleMask = styleMask;
    call.accumulate = accumulate;
    return 0;
  }
  uint8_t resolveStyle(uint8_t style) const { return resolvedStyles[style & 0x03]; }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[4] = {};
  int prewarmCallCount = 0;
  uint8_t resolvedStyles[4] = {0, 1, 2, 3};
};
