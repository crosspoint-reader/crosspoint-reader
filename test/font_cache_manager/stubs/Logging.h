#pragma once

template <typename... Args>
inline void fontCacheManagerTestLog(const Args&...) {}

#define LOG_ERR(...) fontCacheManagerTestLog(__VA_ARGS__)
#define LOG_INF(...) fontCacheManagerTestLog(__VA_ARGS__)
#define LOG_DBG(...) fontCacheManagerTestLog(__VA_ARGS__)
