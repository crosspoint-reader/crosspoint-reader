#pragma once

template <typename... Args>
inline void substackPreviewLog(const Args&...) {}

#define LOG_ERR(...) substackPreviewLog(__VA_ARGS__)
#define LOG_INF(...) substackPreviewLog(__VA_ARGS__)
#define LOG_DBG(...) substackPreviewLog(__VA_ARGS__)
