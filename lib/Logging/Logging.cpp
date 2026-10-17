#include "Logging.h"

#include <BoardConfig.h>
#include <HalClock.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
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

// When true, LOG_* output is withheld from the serial wire (still ring-buffered).
// Single writer (the active transfer activity) / single reader (logPrintf on the
// loop task); a plain volatile bool is sufficient on the 32-bit C3.
static volatile bool serialWireMuted = false;

void setSerialWireMuted(bool muted) { serialWireMuted = muted; }
bool isSerialWireMuted() { return serialWireMuted; }

// Serializes the EMIT half of logPrintf: the transport write and the RTC ring
// append. Formatting stays outside it.
//
// Two tasks logging at once interleaved mid-line on the wire. Device log, X4 Pro:
//
//   [442] [DBG] [GPIO] getWakeupReason: wakeupCause=0, resetReason=11, [442] [INF] [MAIN] Serial log opened
//
// — the GPIO line is cut in half and its tail is lost. Cosmetic on its own, but
// it happens precisely when several tasks are busy, which is when the log is
// worth reading.
//
// The unlogged half is worse: logHead++ is not atomic, so two callers can claim
// the same slot or skip one, corrupting the RTC_NOINIT ring that getLastLogs()
// reads. That buffer exists to explain a crash, so racing it loses the evidence
// exactly when something has gone wrong.
//
// STATIC allocation, deliberately, not xSemaphoreCreateMutex(): logPrintf is
// reachable from global constructors, before setup() runs, and this codebase has
// twice recorded that creating a mutex that early corrupts the TLSF heap
// metadata (ActivityManager::begin, HalStorage::begin say so in as many words).
// A StaticSemaphore_t lives in .bss and touches no heap. The function-local
// static gives one-time, thread-safe initialisation without a second guard.
static SemaphoreHandle_t logMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}

// Whether it is legal to block on that mutex right now. Taking one from an ISR
// is undefined, and from a suspended scheduler (a panic unwind) it cannot
// succeed — in both cases an interleaved line is far better than a fault inside
// the logger. logPrintf is a public entry point, so this is asserted here rather
// than assumed of every caller.
static bool logLockUsable() { return !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING; }

void addToLogRingBuffer(const char* message) {
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
  // add timestamp, wall clock, level and origin
  {
    unsigned long ms = millis();
    char wallClock[12];
    HalClock::formatLogTime(wallClock, sizeof(wallClock));
    int len;
    if (wallClock[0] != '\0') {
      len = snprintf(c, sizeof(buf), "[%lu %s] [%s] [%s] ", ms, wallClock, level, origin);
    } else {
      len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    }
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
    const size_t room = sizeof(buf) - (c - buf);
    int len = vsnprintf(c, room, format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
    // A line cut short at the buffer's end lost the '\n' its format ends in (the LOG_* macros append
    // it), so it ran into the next line on the wire, and getLastLogs() spliced the two into one in
    // a crash report. Keep the line end and lose a character of the message instead.
    // Based on crosspoint-reader PR #2686 (DarkStarDS9, a draft).
    if (static_cast<size_t>(len) >= room) buf[sizeof(buf) - 2] = '\n';
  }
  va_end(args);
  // Everything above is local state (buf, and HalClock::formatLogTime, which is
  // pure). Only the shared state below is serialised, so a slow USB-CDC write
  // never holds up another task's formatting. See logMutex().
  const bool locked = logLockUsable();
  if (locked) xSemaphoreTake(logMutex(), portMAX_DELAY);
  // Log transport, chosen by the board profile (FREEINK_LOG_TRANSPORT).
  //
  // Boards with the same MCU expose logs differently, and getting this wrong is
  // silent: on the LilyGo T5S3 every LOG_* line was dropped because HWCDC's
  // `operator bool` reads false under `pio device monitor`, so the `if
  // (logSerial)` guard below never fired. The board looked mute during bring-up
  // while the Arduino core's own log_e() still came through, which is a
  // thoroughly confusing symptom.
  //
  // serialWireMuted is honoured on every path: it is how the serial-transfer
  // activity keeps log noise out of a binary protocol stream.
  if (!serialWireMuted) {
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
    // IDF/ROM console: the most robust path when the USB bridge is external
    // (Sticky) and Arduino's Serial object is not the thing being monitored.
    esp_rom_printf("%s", buf);
#elif FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_USB_CDC_WRITE
    // Native USB CDC: write unconditionally. Deliberately NOT guarded on
    // `logSerial`, which is the whole point — the readiness check is the bug on
    // this transport, not a safeguard.
    logSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
#else
    // Unguarded for the same reason, on the C3 boards (X4, X3) and the X4 Pro. At the
    // 10 MHz low-power clock FreeRTOS ticks arrive late, the USB Serial/JTAG SOF
    // watchdog briefly reports the host gone, and the next `Serial` check makes HWCDC
    // clear its `connected` flag until a TX interrupt fires. Every line in that window
    // was dropped -- and we drop to 10 MHz after 500 ms idle and during every waveform
    // wait, so the losses clustered around refreshes. Disconnected, HWCDC::write()
    // does not block: it queues into the TX ring (oldest bytes give way) and re-arms
    // the interrupt that sets `connected` again; connected, nothing changes.
    // Ported from crosspoint-reader PR #3737 ("fix: stop dropping log lines at the
    // low-power clock", Sung-jin Brian Hong / @serialx).
    logSerial.print(buf);
#endif
  }
  addToLogRingBuffer(buf);
  if (locked) xSemaphoreGive(logMutex());
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
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
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}
