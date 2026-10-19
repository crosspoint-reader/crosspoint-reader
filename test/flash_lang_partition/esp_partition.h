#pragma once
// An in-memory `spiffs` partition for FlashLangPartition's host tests, with NOR semantics: an erase
// sets whole sectors to 0xFF, a write can only clear bits, and a mapping reads the bytes in place.
// Counters and fault switches live in fake_flash::chip().

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82 } esp_partition_subtype_t;
typedef enum { ESP_PARTITION_MMAP_DATA = 0 } esp_partition_mmap_memory_t;
typedef uint32_t esp_partition_mmap_handle_t;

typedef struct {
  uint32_t address;
  uint32_t size;
} esp_partition_t;

namespace fake_flash {

struct Chip {
  esp_partition_t part{0, 0};
  std::vector<uint8_t> bytes;
  int erases = 0;
  int writes = 0;
  // Writes that start at or past this offset program their first byte with bit 0 the wrong way round:
  // a byte that did not program as written, while the write reports success. ~0u: none.
  size_t corruptWritesFrom = ~static_cast<size_t>(0);
  // Mappings longer than this fail, as when the MMU has no page left. 0: no limit.
  size_t mapLimit = 0;
};

inline Chip& chip() {
  static Chip c;
  return c;
}

inline void reset(const uint32_t size) {
  Chip& c = chip();
  c = Chip{};
  c.part.size = size;
  c.bytes.assign(size, 0xFF);
}

}  // namespace fake_flash

inline const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char*) {
  fake_flash::Chip& c = fake_flash::chip();
  return c.bytes.empty() ? nullptr : &c.part;
}

inline esp_err_t esp_partition_read(const esp_partition_t*, const size_t offset, void* dst, const size_t len) {
  fake_flash::Chip& c = fake_flash::chip();
  if (offset + len > c.bytes.size()) return ESP_FAIL;
  memcpy(dst, c.bytes.data() + offset, len);
  return ESP_OK;
}

inline esp_err_t esp_partition_write(const esp_partition_t*, const size_t offset, const void* src, const size_t len) {
  fake_flash::Chip& c = fake_flash::chip();
  if (offset + len > c.bytes.size()) return ESP_FAIL;
  const auto* in = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < len; ++i) c.bytes[offset + i] &= in[i];
  if (len > 0 && offset >= c.corruptWritesFrom) c.bytes[offset] = static_cast<uint8_t>(in[0] ^ 0x01);
  ++c.writes;
  return ESP_OK;
}

inline esp_err_t esp_partition_erase_range(const esp_partition_t*, const size_t offset, const size_t len) {
  fake_flash::Chip& c = fake_flash::chip();
  if (offset % 4096 || len % 4096 || offset + len > c.bytes.size()) return ESP_FAIL;
  memset(c.bytes.data() + offset, 0xFF, len);
  ++c.erases;
  return ESP_OK;
}

inline esp_err_t esp_partition_mmap(const esp_partition_t*, const size_t offset, const size_t size,
                                    esp_partition_mmap_memory_t, const void** out,
                                    esp_partition_mmap_handle_t* handle) {
  fake_flash::Chip& c = fake_flash::chip();
  if (offset % 65536 || offset + size > c.bytes.size()) return ESP_FAIL;
  if (c.mapLimit && size > c.mapLimit) return ESP_FAIL;
  *out = c.bytes.data() + offset;
  *handle = 1;
  return ESP_OK;
}

inline void esp_partition_munmap(esp_partition_mmap_handle_t) {}
