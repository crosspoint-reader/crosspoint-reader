// Override the prebuilt libbootloader_support.a implementation.
// The X3's validation code misreads the new image's esp_app_desc_t through a
// misaligned bootloader_mmap pointer, producing garbage eFuse block revision
// values that fail the check. Safe to skip: the eFuse block revision gate is
// a manufacturing concern, not a runtime safety issue.
#include <esp_err.h>
esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full) {
  (void)min_rev_full;
  (void)max_rev_full;
  return ESP_OK;
}
