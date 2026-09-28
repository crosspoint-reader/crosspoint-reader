#pragma once

#include <cstdint>
#include <cstdio>

#include "EpdFontFamily.h"

class FontDecompressor {
 public:
  struct PrewarmCall {
    const EpdFontData* fontData = nullptr;
    char text[32] = {};
  };

  void clearCache() {}
  int prewarmCache(const EpdFontData* fontData, const char* text) {
    auto& call = prewarmCalls[prewarmCallCount++];
    call.fontData = fontData;
    std::snprintf(call.text, sizeof(call.text), "%s", text);
    return 0;
  }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[4] = {};
  int prewarmCallCount = 0;
};
