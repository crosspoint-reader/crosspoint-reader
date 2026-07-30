#pragma once

// Host-test stub for lib/Logging/Logging.h, which depends on Arduino's
// HardwareSerial and cannot compile on the host. Logging is a no-op here.

#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#define LOG_DBG(origin, format, ...)
