#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_task_wdt.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);
void __real_panic_arch_fill_info(void* frame, panic_info_t* info);
void __real_panic_soc_fill_info(void* frame, panic_info_t* info);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

// IRAM-safe bounded appends to panicMessage (strncpy and snprintf are not IRAM-safe in panic
// context). Each returns the new length and leaves the message terminated.
static size_t IRAM_ATTR appendPanicText(size_t at, const char* text) {
  while (at < sizeof(panicMessage) - 1 && *text) panicMessage[at++] = *text++;
  panicMessage[at] = '\0';
  return at;
}

static DRAM_ATTR const char HEX_DIGITS[] = "0123456789ABCDEF";
static size_t IRAM_ATTR appendPanicHex(size_t at, const uint32_t value) {
  for (int shift = 28; shift >= 0 && at < sizeof(panicMessage) - 1; shift -= 4) {
    panicMessage[at++] = HEX_DIGITS[(value >> shift) & 0xF];
  }
  panicMessage[at] = '\0';
  return at;
}

void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  appendPanicText(0, message);

  __real_panic_abort(message);
}

// The register names IDF's own register dump uses: the faulting instruction, and what the CPU
// records about the access that faulted (on RISC-V, MTVAL holds the encoding of an illegal
// instruction rather than an address).
#if defined(__riscv)
static DRAM_ATTR const char PANIC_PC_LABEL[] = ", MEPC 0x";
static DRAM_ATTR const char PANIC_FAULT_LABEL[] = ", MTVAL 0x";
#elif defined(__XTENSA__)
static DRAM_ATTR const char PANIC_PC_LABEL[] = ", PC 0x";
static DRAM_ATTR const char PANIC_FAULT_LABEL[] = ", EXCVADDR 0x";
#endif

// For every panic but an abort(), "Panic reason:" in the crash report used to be empty: only an
// abort() passes through panic_abort(). A raw CPU fault or a SoC-level panic goes from the port's
// handler to these two, which turn the exception frame into IDF's panic_info_t; the reason is
// recorded from what they found, with the faulting PC and, for a CPU fault, what the CPU recorded
// about the access. An abort() comes here too (panic_abort() raises a trap to get here), and keeps
// its own message: the trap is not what went wrong.
//
// Based on crosspoint-reader PR #2686 (DarkStarDS9, a draft), which decodes mcause in the
// backtrace hook with a copy of IDF's RISC-V table. Taking IDF's own decoding covers the S3's
// exception causes too, and the SoC-level panics, which the frame alone does not tell apart from
// CPU exceptions on Xtensa. Not esp_panic_handler, which has the same information: the Arduino core
// defines a wrap of it (esp32-hal-misc.c) that this build does not enable, and a second one would
// not link.
static void IRAM_ATTR recordPanicReason(const void* frame, const panic_info_t* info, const bool cpuFault) {
  if (g_panic_abort || !info) return;
  size_t at = appendPanicText(0, info->reason ? info->reason : PANIC_REASON_UNKNOWN);
  at = appendPanicText(at, PANIC_PC_LABEL);
  at = appendPanicHex(at, (uint32_t)info->addr);
  // A SoC-level panic (a watchdog, a cache error) interrupted the code at that PC; the fault
  // register describes no access.
  if (!cpuFault || !frame) return;
#if defined(__riscv)
  const uint32_t faultValue = (uint32_t)((const RvExcFrame*)frame)->mtval;
#elif defined(__XTENSA__)
  const uint32_t faultValue = (uint32_t)((const XtExcFrame*)frame)->excvaddr;
#endif
  at = appendPanicText(at, PANIC_FAULT_LABEL);
  appendPanicHex(at, faultValue);
}

void IRAM_ATTR __wrap_panic_arch_fill_info(void* frame, panic_info_t* info) {
  __real_panic_arch_fill_info(frame, info);
  recordPanicReason(frame, info, true);
}

void IRAM_ATTR __wrap_panic_soc_fill_info(void* frame, panic_info_t* info) {
  __real_panic_soc_fill_info(frame, info);
  recordPanicReason(frame, info, false);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // The exception frame is architecture-specific and keeps the stack pointer in a
  // different field: RISC-V in `sp`, Xtensa in `a1` ("stack pointer before
  // interrupt"). Only that extraction differs — the raw stack walk below is
  // architecture-neutral, since it just dumps words upward from the frame.
  // Frame layouts per components/esp_system/port/arch/{riscv,xtensa}/panic_arch.c.
#if defined(__riscv)
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
#elif defined(__XTENSA__)
  uint32_t sp = (uint32_t)((XtExcFrame*)frame)->a1;
#else
#error "Unknown architecture: no exception-frame stack pointer mapping."
#endif
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    info += "\n\nPanic reason: " + std::string(panicMessage);
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  // Watchdog resets count as panics here: both watchdogs panic on timeout in this
  // build (CONFIG_ESP_TASK_WDT_PANIC, CONFIG_ESP_INT_WDT), so they preserve the same
  // panic info and deserve the same crash report. Without these a wedged task just
  // rebooted silently to Home with no diagnostic trail.
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP || resetReason == ESP_RST_INT_WDT ||
         resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
}

void feedWatchdog() {
  // ESP_OK means this task is subscribed; anything else (ESP_ERR_NOT_FOUND, or
  // the watchdog not being initialised at all) means there is nothing to feed
  // and calling reset would only log an error. See the header for the cost.
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
  }
}

}  // namespace HalSystem
