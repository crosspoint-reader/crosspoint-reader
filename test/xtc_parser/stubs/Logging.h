#pragma once
inline void xtcTestLog(const char*, const char*, ...) {}
#define LOG_ERR(...) xtcTestLog(__VA_ARGS__)
#define LOG_INF(...) xtcTestLog(__VA_ARGS__)
#define LOG_DBG(...) xtcTestLog(__VA_ARGS__)
