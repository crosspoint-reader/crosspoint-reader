#pragma once

template <typename... Args>
inline void readerFontSizesTestLog(const Args&...) {}

#define LOG_ERR(...) readerFontSizesTestLog(__VA_ARGS__)
#define LOG_INF(...) readerFontSizesTestLog(__VA_ARGS__)
#define LOG_DBG(...) readerFontSizesTestLog(__VA_ARGS__)
