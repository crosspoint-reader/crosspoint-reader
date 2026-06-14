#pragma once

// Host stand-in for the firmware <Logging.h> (which pulls in Arduino HardwareSerial).
// Logging is irrelevant to parser behavior, so the macros are no-ops in tests.
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
