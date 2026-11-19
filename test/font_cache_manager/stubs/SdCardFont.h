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
  void beginPrewarmScope() { scannedStyleMask_ = 0; }
  void releaseResidentCaches() { scannedStyleMask_ = 0; }
  void noteScannedStyle(uint8_t style) { scannedStyleMask_ |= static_cast<uint8_t>(1u << resolveStyle(style)); }
  uint8_t scannedStyleMask() const { return scannedStyleMask_; }
  void compactForRender(uint8_t styleMask, bool dropLayoutAdvanceCache) {
    lastCompactStyleMask = styleMask;
    lastCompactDropAdvance = dropLayoutAdvanceCache;
    compactCallCount++;
  }
  int prewarm(const char* text, uint8_t styleMask, bool, bool, bool accumulate) {
    if (prewarmCallCount < 8) {
      auto& call = prewarmCalls[prewarmCallCount];
      std::snprintf(call.text, sizeof(call.text), "%s", text);
      call.styleMask = styleMask;
      call.accumulate = accumulate;
    }
    prewarmCallCount++;
    return 0;
  }
  uint8_t resolveStyle(uint8_t style) const { return resolvedStyles[style & 0x03]; }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[8] = {};
  int prewarmCallCount = 0;
  uint8_t resolvedStyles[4] = {0, 1, 2, 3};
  uint8_t lastCompactStyleMask = 0xFF;
  bool lastCompactDropAdvance = false;
  int compactCallCount = 0;

 private:
  uint8_t scannedStyleMask_ = 0;
};
