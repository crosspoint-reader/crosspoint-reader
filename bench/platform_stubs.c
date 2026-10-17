// Minimal wrap stubs required by the ESP32-C3 linker when
// -Wl,--wrap=panic_print_backtrace,--wrap=panic_abort,--wrap=panic_arch_fill_info,
//    --wrap=panic_soc_fill_info,--wrap=bootloader_common_check_efuse_blk_validity
// is in build_flags. The bench sketch doesn't need panic capture or efuse
// checking, so we just forward to the real implementations.

#include <stdint.h>

#include "esp_err.h"

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);
void __real_panic_arch_fill_info(void* frame, void* info);
void __real_panic_soc_fill_info(void* frame, void* info);

void __wrap_panic_abort(const char* message) { __real_panic_abort(message); }

void __wrap_panic_print_backtrace(const void* frame, int core) { __real_panic_print_backtrace(frame, core); }

void __wrap_panic_arch_fill_info(void* frame, void* info) { __real_panic_arch_fill_info(frame, info); }

void __wrap_panic_soc_fill_info(void* frame, void* info) { __real_panic_soc_fill_info(frame, info); }

esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full) {
  (void)min_rev_full;
  (void)max_rev_full;
  return 0;  // ESP_OK
}
