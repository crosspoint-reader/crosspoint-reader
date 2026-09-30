#pragma once
inline void inflateTestLog(const char*, const char*, ...) {}
#define LOG_ERR(...) inflateTestLog(__VA_ARGS__)
