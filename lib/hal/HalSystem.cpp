#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

#if __riscv
// Mirrors the exception-code table in esp-idf's panic_arch_fill_info()
// (components/esp_system/port/arch/riscv/panic_arch.c), indexed by the RISC-V `mcause` CSR value found
// in the panic frame. Only standard architectural exceptions are covered; SoC-level pseudo-causes
// (watchdog timeout, cache error) use out-of-range mcause sentinel values from separate ETS_*_INUM
// tables and are intentionally left undecoded (nullptr) rather than guessed at.
static DRAM_ATTR const char PANIC_MCAUSE_0[] = "Instruction address misaligned";
static DRAM_ATTR const char PANIC_MCAUSE_1[] = "Instruction access fault";
static DRAM_ATTR const char PANIC_MCAUSE_2[] = "Illegal instruction";
static DRAM_ATTR const char PANIC_MCAUSE_3[] = "Breakpoint";
static DRAM_ATTR const char PANIC_MCAUSE_4[] = "Load address misaligned";
static DRAM_ATTR const char PANIC_MCAUSE_5[] = "Load access fault";
static DRAM_ATTR const char PANIC_MCAUSE_6[] = "Store address misaligned";
static DRAM_ATTR const char PANIC_MCAUSE_7[] = "Store access fault";
static DRAM_ATTR const char PANIC_MCAUSE_8[] = "Environment call from U-mode";
static DRAM_ATTR const char PANIC_MCAUSE_9[] = "Environment call from S-mode";
static DRAM_ATTR const char PANIC_MCAUSE_11[] = "Environment call from M-mode";
static DRAM_ATTR const char PANIC_MCAUSE_12[] = "Instruction page fault";
static DRAM_ATTR const char PANIC_MCAUSE_13[] = "Load page fault";
static DRAM_ATTR const char PANIC_MCAUSE_15[] = "Store page fault";
static DRAM_ATTR const char* const PANIC_MCAUSE_REASONS[] = {
    PANIC_MCAUSE_0,  PANIC_MCAUSE_1,  PANIC_MCAUSE_2, PANIC_MCAUSE_3,  PANIC_MCAUSE_4, PANIC_MCAUSE_5,
    PANIC_MCAUSE_6,  PANIC_MCAUSE_7,  PANIC_MCAUSE_8, PANIC_MCAUSE_9,  nullptr,        PANIC_MCAUSE_11,
    PANIC_MCAUSE_12, PANIC_MCAUSE_13, nullptr,        PANIC_MCAUSE_15,
};
#endif

void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  // Raw architectural faults (LoadProhibited, StoreProhibited, illegal instruction, ...) never call
  // __wrap_panic_abort above -- only an explicit abort()/assert() does. Backfill panicMessage from the
  // frame's mcause exception code here (this hook runs unconditionally for every panic type, per
  // esp-idf's print_state() in port/panic_handler.c), so those crash types still get a human-readable
  // reason in the crash report instead of staying blank.
  if (panicMessage[0] == '\0') {
    uint32_t mcause = ((RvExcFrame*)frame)->mcause;
    const char* reason = mcause < (sizeof(PANIC_MCAUSE_REASONS) / sizeof(PANIC_MCAUSE_REASONS[0]))
                             ? PANIC_MCAUSE_REASONS[mcause]
                             : nullptr;
    if (reason) {
      int i = 0;
      for (; i < (int)sizeof(panicMessage) - 1 && reason[i]; i++) {
        panicMessage[i] = reason[i];
      }
      panicMessage[i] = '\0';
    }
  }

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
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
#endif
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
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP;
}

}  // namespace HalSystem
