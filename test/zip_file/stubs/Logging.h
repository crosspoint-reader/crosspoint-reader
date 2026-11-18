#pragma once
inline void zipTestLog(const char*, const char*, ...) {}
#define LOG_ERR(...) zipTestLog(__VA_ARGS__)
#define LOG_DBG(...) zipTestLog(__VA_ARGS__)
