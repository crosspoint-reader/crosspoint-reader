#pragma once

#include <cstdio>
#include <string>

inline bool sdFontTestCaptureLogs = false;
inline std::string sdFontTestLogs;

template <typename... Args>
inline void fontCacheManagerTestLog(const char*, const char* format, Args... args) {
  if (!sdFontTestCaptureLogs) return;
  if constexpr (sizeof...(args) == 0) {
    sdFontTestLogs += format;
  } else {
    char message[512];
    std::snprintf(message, sizeof(message), format, args...);
    sdFontTestLogs += message;
  }
  sdFontTestLogs += '\n';
}

#define LOG_ERR(...) fontCacheManagerTestLog(__VA_ARGS__)
#define LOG_INF(...) fontCacheManagerTestLog(__VA_ARGS__)
#define LOG_DBG(...) fontCacheManagerTestLog(__VA_ARGS__)
