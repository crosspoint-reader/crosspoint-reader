#pragma once
template <typename... Args>
inline void testLog(const char*, const char*, Args...) {}
#define LOG_DBG(...) testLog(__VA_ARGS__)
#define LOG_ERR(...) testLog(__VA_ARGS__)
#define LOG_INF(...) testLog(__VA_ARGS__)
