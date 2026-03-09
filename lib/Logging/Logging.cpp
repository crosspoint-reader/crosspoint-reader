#include "Logging.h"

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash.
// Stored in RTC memory so entries survive a panic-induced reset.
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;

// Spinlock protecting concurrent reads/writes from main and render tasks.
// portMUX is ISR-safe and does not allocate heap, making it compatible with
// the RTC_NOINIT context and usable from any FreeRTOS task priority.
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary
  portENTER_CRITICAL(&logMux);
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
  portEXIT_CRITICAL(&logMux);
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  if (!logSerial) {
    return;  // Serial not initialized, skip logging
  }
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add the timestamp
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] ", ms);
    if (len < 0) {
      return;  // encoding error, skip logging
    }
    c += len;
  }
  // add the level
  {
    const char* p = level;
    size_t remaining = sizeof(buf) - (c - buf);
    while (*p && remaining > 1) {
      *c++ = *p++;
      remaining--;
    }
    if (remaining > 1) {
      *c++ = ' ';
    }
  }
  // add the origin
  {
    int len = snprintf(c, sizeof(buf) - (c - buf), "[%s] ", origin);
    if (len < 0) {
      return;  // encoding error, skip logging
    }
    c += len;
  }
  // add the user message
  vsnprintf(c, sizeof(buf) - (c - buf), format, args);
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  std::string output;
  portENTER_CRITICAL(&logMux);
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      output += logMessages[idx];
    }
  }
  portEXIT_CRITICAL(&logMux);
  return output;
}

void clearLastLogs() {
  portENTER_CRITICAL(&logMux);
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  portEXIT_CRITICAL(&logMux);
}
