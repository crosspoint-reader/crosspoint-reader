#include "Logging.h"

#include <BoardConfig.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

// LOG_* is called from multiple FreeRTOS tasks (main loop task + ActivityManager's render task, see
// ActivityManager::renderTaskLoop()) without any prior synchronization. Two calls racing on the same
// logHead slot -- one preempted mid-strncpy by the other -- can leave a shorter new message's write
// interrupted before its null-padding finishes clearing the slot, so a stale tail from the previous
// occupant survives right after it. That produced exactly this in a real crash report:
//   "Time = 141 ms from clearScreen to displayBuffers from clearScreen to displayBuffer"
// (the "s from clearScreen to displayBuffer" tail is leftover from a longer earlier message in the same
// slot, not truncation -- the message itself is far under MAX_ENTRY_LEN). This mutex serializes all
// ring-buffer access. Never called from ISR context (grep for LOG_* inside IRAM_ATTR functions turns up
// only HalSystem's non-IRAM checkPanic()), so a plain (non-ISR) mutex is sufficient.
static SemaphoreHandle_t logMutex = xSemaphoreCreateMutex();

namespace {
// RAII helper; falls back to no locking if logMutex hasn't been constructed yet (only possible if some
// other translation unit's global constructor calls a LOG_* macro before this file's own global
// initializer has run -- C++ does not guarantee cross-TU init order).
struct LogLock {
  bool locked = false;
  LogLock() {
    if (logMutex) {
      xSemaphoreTake(logMutex, portMAX_DELAY);
      locked = true;
    }
  }
  ~LogLock() {
    if (locked) {
      xSemaphoreGive(logMutex);
    }
  }
};
}  // namespace

void addToLogRingBuffer(const char* message) {
  LogLock lock;
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
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
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
  // IDF/ROM console path for boards monitored over USB-Serial-JTAG, where the
  // HWCDC `operator bool` reads false under `pio device monitor` and logs would
  // otherwise be silently dropped (e.g. Sticky).
  esp_rom_printf("%s", buf);
#else
  if (logSerial) {
    logSerial.print(buf);
  }
#endif
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  LogLock lock;
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      // logPrintf's format string ends in "\n", but if the formatted prefix+message exceeded
      // MAX_ENTRY_LEN, vsnprintf truncates and that trailing '\n' can be the part that's cut off. If we
      // just concatenated entries as-is, a truncated entry would run straight into the next one with no
      // separator, looking like two log lines spliced/interleaved into one. Strip any trailing
      // newline(s) and always append exactly one, so every stored entry lands on its own line
      // regardless of whether it was truncated.
      while (len > 0 && (logMessages[idx][len - 1] == '\n' || logMessages[idx][len - 1] == '\r')) {
        len--;
      }
      output.append(logMessages[idx], len);
      output += '\n';
    }
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
bool sanitizeLogHead() {
  LogLock lock;
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  LogLock lock;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}
