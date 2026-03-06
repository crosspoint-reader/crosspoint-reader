#include "Logging.h"

#include <string>

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
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
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      output += logMessages[idx];
    }
  }
  return output;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
}
