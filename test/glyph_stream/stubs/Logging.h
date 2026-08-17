#pragma once

template <typename... Args>
inline void glyphStreamTestLog(const Args&...) {}

#define LOG_ERR(...) glyphStreamTestLog(__VA_ARGS__)
#define LOG_INF(...) glyphStreamTestLog(__VA_ARGS__)
#define LOG_DBG(...) glyphStreamTestLog(__VA_ARGS__)
