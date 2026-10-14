#include "EspNorFlash.h"

#include <Logging.h>
#include <spi_flash_mmap.h>

#include <cstring>

#include "FlashFontPartition.h"

#if CONFIG_SPIRAM
#include <esp_memory_utils.h>
#endif

namespace {
uint32_t s_liveMappings = 0;
}

EspNorFlash::~EspNorFlash() {
  for (Mapping& m : maps_) {
    if (m.ptr != nullptr) unmap(m.ptr);
  }
}

uint32_t EspNorFlash::liveMappings() { return s_liveMappings; }

bool EspNorFlash::begin() {
  part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part_) {
    LOG_ERR("ENF", "spiffs partition not found");
    return false;
  }
  size_ = static_cast<uint32_t>(FlashFontPartition::fontUsableSize());
  return size_ > 0;
}

bool EspNorFlash::read(const uint32_t offset, void* dst, const uint32_t len) const {
  if (!inRange(offset, len)) return false;
  return esp_partition_read(part_, offset, dst, len) == ESP_OK;
}

bool EspNorFlash::writable(const char* what, const uint32_t offset, const uint32_t len) const {
  if (!inRange(offset, len)) {
    LOG_ERR("ENF", "%s of %u B at %u is outside the font area (%u B)", what, static_cast<unsigned>(len),
            static_cast<unsigned>(offset), static_cast<unsigned>(size_));
    return false;
  }
  if (FlashFontPartition::isMapped() || FlashFontPartition::isWriting()) {
    LOG_ERR("ENF", "%s refused: a .cpfont is mapped from, or being written to, this partition", what);
    return false;
  }
  return true;
}

bool EspNorFlash::write(const uint32_t offset, const void* src, const uint32_t len) {
  const esp_err_t err = esp_partition_write(part_, offset, src, len);
  if (err != ESP_OK) {
    LOG_ERR("ENF", "program of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

bool EspNorFlash::program(const uint32_t offset, const void* src, const uint32_t len) {
  if (!writable("program", offset, len)) return false;
#if CONFIG_SPIRAM
  if (!esp_ptr_internal(src)) {
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (uint32_t done = 0; done < len;) {
      const uint32_t chunk = len - done < sizeof bounce_ ? len - done : static_cast<uint32_t>(sizeof bounce_);
      std::memcpy(bounce_, bytes + done, chunk);
      if (!write(offset + done, bounce_, chunk)) return false;
      done += chunk;
    }
    return true;
  }
#endif
  return write(offset, src, len);
}

bool EspNorFlash::erase(const uint32_t offset, const uint32_t len) {
  if (!writable("erase", offset, len)) return false;
  if (offset % kSectorBytes != 0 || len % kSectorBytes != 0) {
    LOG_ERR("ENF", "erase of %u B at %u is not sector-aligned", static_cast<unsigned>(len),
            static_cast<unsigned>(offset));
    return false;
  }
  const esp_err_t err = esp_partition_erase_range(part_, offset, len);
  if (err != ESP_OK) {
    LOG_ERR("ENF", "erase of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

const uint8_t* EspNorFlash::map(const uint32_t offset, const uint32_t len) {
  if (len == 0 || !inRange(offset, len)) return nullptr;
  for (Mapping& m : maps_) {
    if (m.ptr != nullptr) continue;
    const void* ptr = nullptr;
    const esp_err_t err = esp_partition_mmap(part_, offset, len, ESP_PARTITION_MMAP_DATA, &ptr, &m.handle);
    if (err != ESP_OK) {
      LOG_ERR("ENF", "map of %u B at %u failed: %d (%u pages free)", static_cast<unsigned>(len),
              static_cast<unsigned>(offset), err, static_cast<unsigned>(freeMapPages()));
      m = Mapping{};
      return nullptr;
    }
    m.ptr = static_cast<const uint8_t*>(ptr);
    ++s_liveMappings;
    return m.ptr;
  }
  LOG_ERR("ENF", "map refused: all %d mappings are in use", kMaxMaps);
  return nullptr;
}

void EspNorFlash::unmap(const uint8_t* mapped) {
  if (mapped == nullptr) return;
  for (Mapping& m : maps_) {
    if (m.ptr == mapped) {
      esp_partition_munmap(m.handle);
      m = Mapping{};
      --s_liveMappings;
      return;
    }
  }
}

uint32_t EspNorFlash::freeMapPages() const {
  return static_cast<uint32_t>(spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
}
