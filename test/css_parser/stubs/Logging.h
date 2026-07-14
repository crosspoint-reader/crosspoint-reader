#pragma once

// Host-test stub for lib/Logging/Logging.h, which depends on Arduino's
// HardwareSerial and cannot compile on the host. Logging is a no-op here, but
// the macros still consume their arguments so variables used only in log
// statements don't trigger -Wunused-but-set-variable in the test build.

namespace logging_stub {
inline void sink(const char*, ...) {}
}  // namespace logging_stub

#define LOG_ERR(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INF(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
#define LOG_DBG(origin, format, ...) logging_stub::sink(origin, format __VA_OPT__(, ) __VA_ARGS__)
