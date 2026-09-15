#pragma once

#include <cstdarg>
#include <cstdio>

namespace documentIdTestLogging {
inline void logError(const char* tag, const char* format, ...) {
  std::fprintf(stderr, "[%s] ", tag);
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fputc('\n', stderr);
}
}  // namespace documentIdTestLogging

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) documentIdTestLogging::logError(__VA_ARGS__)
