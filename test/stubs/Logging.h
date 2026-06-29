#pragma once
// Host-test stub for lib/Logging/Logging.h (device-only — includes Arduino
// HardwareSerial). The renderer only uses LOG_ERR on file-open failure; on the
// host these are no-ops.
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) ((void)0)
#define LOG_DBG(origin, format, ...) ((void)0)
