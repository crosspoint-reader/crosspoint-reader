#pragma once

#include <string>

extern "C" {

void __real_panic_abort(char* message);
void __wrap_panic_abort(char* message);

void __real_panic_print_backtrace(const void *frame, int core);
void __wrap_panic_print_backtrace(const void *frame, int core);

}

namespace HalSystem {
  struct StackFrame {
    uint32_t sp;
    uint32_t spp[8];
  };

  // Dump panic info to SD card if necessary
  void checkPanic();

  std::string getPanicInfo(bool full = false);
  bool isRebootFromPanic();
}
