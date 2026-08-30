#pragma once

namespace logging_stub {
inline void sink(const char*, ...) {}
}  // namespace logging_stub

#define LOG_ERR(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INF(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
#define LOG_DBG(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
