#include "Logging.h"

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16
#define LOG_INIT_MAGIC 0xA55A3CC3u

// Simple ring buffer log, useful for error reporting when we encounter a crash.
// Stored in RTC memory so entries survive a panic-induced reset.
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic value confirming the ring buffer has been explicitly initialized.
// Without this, a cold power-on leaves logMessages filled with garbage.
RTC_NOINIT_ATTR uint32_t logInitMagic = 0;

// Spinlock protecting concurrent reads/writes from main and render tasks.
// portMUX is ISR-safe and does not allocate heap, making it compatible with
// the RTC_NOINIT context and usable from any FreeRTOS task priority.
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// Advance pointer by `len` bytes, clamping so it never exceeds `end`.
static inline char* clamp_advance(char* c, int len, const char* end) {
  if (len <= 0) return c;
  if (c >= end) return const_cast<char*>(end);
  size_t avail = static_cast<size_t>(end - c);
  return c + (static_cast<size_t>(len) < avail ? static_cast<size_t>(len) : avail);
}

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // On a cold boot, RTC_NOINIT memory is garbage until clearLastLogs() runs.
  // Reinitialize here if the magic is missing or logHead is out of bounds to
  // prevent an out-of-bounds write into logMessages.
  portENTER_CRITICAL(&logMux);
  if (logInitMagic != LOG_INIT_MAGIC || logHead >= MAX_LOG_LINES) {
    for (size_t i = 0; i < MAX_LOG_LINES; i++) {
      logMessages[i][0] = '\0';
    }
    logHead = 0;
    logInitMagic = LOG_INIT_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
  portEXIT_CRITICAL(&logMux);
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  const char* end = buf + sizeof(buf);
  // add the timestamp
  {
    unsigned long ms = millis();
    int len = snprintf(c, static_cast<size_t>(end - c), "[%lu] ", ms);
    if (len < 0) {
      va_end(args);
      return;  // encoding error, skip logging
    }
    c = clamp_advance(c, len, end);
  }
  // add the level
  {
    const char* p = level;
    size_t remaining = static_cast<size_t>(end - c);
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
    int len = snprintf(c, static_cast<size_t>(end - c), "[%s] ", origin);
    if (len < 0) {
      va_end(args);
      return;  // encoding error, skip logging
    }
    c = clamp_advance(c, len, end);
  }
  // add the user message
  vsnprintf(c, static_cast<size_t>(end - c), format, args);
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  // Only return logs if the ring buffer has been properly initialized.
  // Without this check a cold-boot (RTC_NOINIT is garbage) would read
  // random bytes and potentially walk off the end of a slot.
  if (logInitMagic != LOG_INIT_MAGIC) {
    return {};
  }

  // Snapshot under a dedicated mutex so each caller gets its own isolated copy.
  // We use a second portMUX (snapshotMux) to serialize concurrent callers of
  // getLastLogs() without holding logMux (which would block the logging ISR
  // path) during the heap work that follows.
  // Heap-allocate snapshot to avoid 4KB stack usage (16 * 256) which can
  // overflow ESP32 task stacks (typically 4-8KB).
  auto* snapshot = static_cast<char(*)[MAX_ENTRY_LEN]>(malloc(MAX_LOG_LINES * MAX_ENTRY_LEN));
  if (!snapshot) {
    return {};
  }

  static portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&snapshotMux);
  size_t snapHead = 0;
  portENTER_CRITICAL(&logMux);
  memcpy(snapshot, logMessages, MAX_LOG_LINES * MAX_ENTRY_LEN);
  snapHead = logHead;
  portEXIT_CRITICAL(&logMux);

  std::string output;
  output.reserve(MAX_LOG_LINES * MAX_ENTRY_LEN);
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (snapHead + i) % MAX_LOG_LINES;
    if (snapshot[idx][0] != '\0') {
      output += snapshot[idx];
    }
  }
  portEXIT_CRITICAL(&snapshotMux);
  free(snapshot);
  return output;
}

void clearLastLogs() {
  portENTER_CRITICAL(&logMux);
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  logInitMagic = LOG_INIT_MAGIC;
  portEXIT_CRITICAL(&logMux);
}
