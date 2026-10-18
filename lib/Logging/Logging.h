#pragma once

#include <HardwareSerial.h>

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
3 = ERR + INF + DBG + TRC
If not defined, defaults to 0

Choosing a level for a new log line:

  ERR  something went wrong, or a degraded path was taken that the user may notice.
  INF  a state change worth seeing in a release build: activity transitions, build
       lifecycle, ownership of a scarce resource (the secondary framebuffer).
  DBG  ONE line per user-visible event — a page rendered, a section built, an image
       decoded. This is the level a developer reads while working, so it has to stay
       readable: if a normal page turn adds more than a handful of lines, they belong
       one level down.
  TRC  per-item detail *inside* one of those events: per image, per line, per glyph
       group, per element. Individually useful when debugging that one subsystem,
       collectively the thing that makes a trace impossible to read. Compiled out at
       the default development level (2) and re-enabled with -DLOG_LEVEL=3.

TRC exists because the alternative was a per-subsystem #define for each area anyone
had ever debugged, each defaulting to on because turning it off felt like losing
something. One dial, nothing deleted.

If you have a legitimate need for raw Serial access (e.g., binary data,
special formatting), use the underlying logSerial object directly:
    logSerial.printf("Special case: %d\n", value);
    logSerial.write(binaryData, length);

The logSerial reference (defined below) points to the real Serial object and
won't trigger deprecation warnings.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

static HWCDC& logSerial = Serial;

void logPrintf(const char* level, const char* origin, const char* format, ...);

// Mute/unmute LOG_* output to the serial wire. While muted, log lines are still
// recorded in the RTC ring buffer (getLastLogs) but are NOT written to
// logSerial. Used during binary serial file transfers so a concurrent task's
// log bytes can't interleave with the protocol's 0x06 ACKs / payload and
// corrupt the stream. Direct logSerial.write() (e.g. the transfer itself) is
// unaffected — only the LOG_* path is gated. Mirrors MicroReader's
// esp_log_level_set("*", ESP_LOG_NONE) during uploads.
void setSerialWireMuted(bool muted);
bool isSerialWireMuted();

// The most verbose level written to the serial wire at run time (0 = ERR .. 3 = TRC; the serial
// CMD:LOG sets it). LOG_LEVEL still decides what is compiled in, so this can only quieten the
// wire; it starts at LOG_LEVEL. The RTC ring buffer keeps every line either way.
void setLogWireLevel(uint8_t level);
uint8_t logWireLevel();

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif

#if LOG_LEVEL >= 3
#define LOG_TRC(origin, format, ...) logPrintf("TRC", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_TRC(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_TRC(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();
// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud) { logSerial.begin(baud); }

  // Support boolean conversion for compatibility with code like:
  //   if (Serial) or while (!Serial)
  operator bool() const { return logSerial; }

  __attribute__((deprecated("Use LOG_* macro instead"))) size_t printf(const char* format, ...);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};

#ifdef Serial
#undef Serial
#endif
#define Serial MySerialImpl::instance
