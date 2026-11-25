// ESP32-S31 Bluetooth memory, for the page turner's stack (Metalio Plus). The prebuilt
// Arduino libbt.a holds ESP-IDF's NimBLE host and the controller, built for internal RAM;
// on the S31 that is the RAM Wi-Fi, the JPEG decoder and the controller's own buffers
// compete for. The Plus environment links them with -Wl,--wrap (platformio.ini):
//  * nimble_mem_malloc/calloc/realloc, through which every host allocation goes, take
//    PSRAM first, as CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL would (the S3 gets that
//    from scripts/s3_nimble_psram.h, which compiles its host itself).
//  * wr_btdm_osal_malloc honours the controller's BTDM_OSAL_MALLOC_F_ALLOW_SPIRAM flag
//    on the blocks it marks safe for PSRAM, as CONFIG_BT_CTRL_ALLOW_MALLOC_FROM_SPIRAM
//    would. Its other blocks stay in internal DMA-capable RAM.
// Each falls back to the stock allocator. Measured 02/11/2026: a radio start took
// 35,340 B of internal RAM with these, and could not finish its scan buffers without.
#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S31 && FREEINK_CAP_BLE_HID_HOST

#include <esp_heap_caps.h>

#include <cstddef>
#include <cstdint>

namespace {
constexpr uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kBtdmAllowSpiram = 1u << 0;  // BTDM_OSAL_MALLOC_F_ALLOW_SPIRAM
}  // namespace

extern "C" {
void* __real_nimble_mem_malloc(size_t size);
void* __real_nimble_mem_calloc(size_t count, size_t size);
void* __real_nimble_mem_realloc(void* ptr, size_t size);
void* __real_wr_btdm_osal_malloc(uint32_t size, uint32_t flags);

void* __wrap_nimble_mem_malloc(const size_t size) {
  void* p = heap_caps_malloc(size, kPsram);
  return p ? p : __real_nimble_mem_malloc(size);
}

void* __wrap_nimble_mem_calloc(const size_t count, const size_t size) {
  void* p = heap_caps_calloc(count, size, kPsram);
  return p ? p : __real_nimble_mem_calloc(count, size);
}

void* __wrap_nimble_mem_realloc(void* ptr, const size_t size) {
  void* p = heap_caps_realloc(ptr, size, kPsram);
  return p ? p : __real_nimble_mem_realloc(ptr, size);
}

void* __wrap_wr_btdm_osal_malloc(const uint32_t size, const uint32_t flags) {
  void* p = (flags & kBtdmAllowSpiram) ? heap_caps_malloc(size, kPsram) : nullptr;
  return p ? p : __real_wr_btdm_osal_malloc(size, flags);
}
}

#endif
