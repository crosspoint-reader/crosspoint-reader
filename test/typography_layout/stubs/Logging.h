#pragma once

inline void discardTestLog(const char*, ...) {}

#define LOG_ERR(tag, ...) discardTestLog(tag, __VA_ARGS__)
#define LOG_DBG(tag, ...) discardTestLog(tag, __VA_ARGS__)
#define LOG_INF(tag, ...) discardTestLog(tag, __VA_ARGS__)
#define LOG_WRN(tag, ...) discardTestLog(tag, __VA_ARGS__)
