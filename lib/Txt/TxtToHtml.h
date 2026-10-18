#pragma once

#include <Print.h>

#include <string_view>

class TxtToHtml {
 public:
  static const char* cacheVersionTag(std::string_view filename);
  static bool stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                     Print& out);
  static bool stream(std::string_view filename, std::string_view content, Print& out);
};
