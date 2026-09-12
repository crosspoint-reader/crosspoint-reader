#pragma once

void tiltTestLog(const char* level, const char* origin, const char* format, ...);

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 0
#define LOG_ERR(origin, ...) tiltTestLog("ERR", origin, __VA_ARGS__)
#else
#define LOG_ERR(...) ((void)0)
#endif

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 1
#define LOG_INF(origin, ...) tiltTestLog("INF", origin, __VA_ARGS__)
#else
#define LOG_INF(...) ((void)0)
#endif

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
#define LOG_DBG(origin, ...) tiltTestLog("DBG", origin, __VA_ARGS__)
#else
#define LOG_DBG(...) ((void)0)
#endif
