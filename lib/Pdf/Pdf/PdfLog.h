#pragma once

#include <Logging.h>

#if defined(ENABLE_SERIAL_LOG)
inline void pdfLogLine(const char* level, const char* message) {
  logSerial.print(level);
  logSerial.print(" [PDF] ");
  logSerial.println(message);
}

inline void pdfLogErr(const char* message) { pdfLogLine("[ERR]", message); }
inline void pdfLogDbg(const char* message) { pdfLogLine("[DBG]", message); }

inline void pdfLogErrU32(const char* message, uint32_t value) {
  logSerial.print("[ERR] [PDF] ");
  logSerial.print(message);
  logSerial.println(value);
}

inline void pdfLogErrPath(const char* message, const char* path) {
  logSerial.print("[ERR] [PDF] ");
  logSerial.print(message);
  logSerial.println(path);
}
#else
inline void pdfLogErr(const char*) {}
inline void pdfLogDbg(const char*) {}
inline void pdfLogErrU32(const char*, uint32_t) {}
inline void pdfLogErrPath(const char*, const char*) {}
#endif
