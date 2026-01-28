#include "ResourcesFS.h"

#include <Arduino.h>
#include <esp_partition.h>

#include <vector>

ResourcesFS ResourcesFS::instance;

class ResourcesFS::Impl {
 public:
  const esp_partition_t* partition = nullptr;
  const Header* header = nullptr;
  const uint8_t* mmap_data = nullptr;
};

bool ResourcesFS::begin(bool remount) {
  if (!remount) {
    assert(impl == nullptr && "begin called multiple times");
    impl = new Impl();
  } else {
    assert(impl != nullptr && "remount called before initial begin");
  }

  impl->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!impl->partition) {
    Serial.printf("[%lu] [FSS] SPIFFS partition not found, skipping\n", millis());
    impl->header = nullptr;
    impl->mmap_data = nullptr;
    return false;
  }

  spi_flash_mmap_handle_t map_handle;  // unused
  size_t len = impl->partition->size;
  if (len > MAX_ALLOC_SIZE) {
    len = MAX_ALLOC_SIZE;
  }

  auto err =
      esp_partition_mmap(impl->partition, 0, len, SPI_FLASH_MMAP_DATA, (const void**)&impl->mmap_data, &map_handle);
  if (err != ESP_OK || impl->mmap_data == nullptr) {
    Serial.printf("[%lu] [FSS] mmap failed, code: %d, skipping\n", millis(), err);
    impl->header = nullptr;
    impl->mmap_data = nullptr;
    return false;
  }

  impl->header = (const Header*)impl->mmap_data;
  if (impl->header->magic != MAGIC) {
    Serial.printf("[%lu] [FSS] Invalid magic: 0x%08X, skipping\n", millis(), impl->header->magic);
    impl->header = nullptr;
    impl->mmap_data = nullptr;
    return false;
  }

  Serial.printf("[%lu] [FSS] ResourcesFS initialized\n", millis());
  return true;
}

const ResourcesFS::Header* ResourcesFS::getRoot() {
  assert(impl != nullptr);
  return impl->header;
}

const uint8_t* ResourcesFS::mmap(const ResourcesFS::FileEntry* entry) {
  assert(impl != nullptr);
  assert(impl->header != nullptr);
  assert(impl->mmap_data != nullptr);

  size_t offset = sizeof(Header);
  for (size_t i = 0; i < MAX_FILES; i++) {
    const FileEntry& e = impl->header->entries[i];
    if (&e == entry) {
      break;
    }
    offset += e.size;
    offset += get_padding(e.size);
  }
  return impl->mmap_data + offset;
}

bool ResourcesFS::erase(size_t size) {
  assert(impl != nullptr);
  assert(impl->partition != nullptr);

  // align size to sector size
  static constexpr size_t sector_size = 4096;
  size = (size + sector_size - 1) / sector_size * sector_size;

  auto err = esp_partition_erase_range(impl->partition, 0, size);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [FSS] erase failed, code %d\n", millis(), err);
    return false;
  }

  return true;
}

bool ResourcesFS::write(uint32_t offset, const uint8_t* data, size_t len) {
  assert(impl != nullptr);
  assert(impl->partition != nullptr);
  assert(offset + len <= impl->partition->size);

  auto err = esp_partition_write(impl->partition, offset, data, len);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [FSS] write failed, offset %u, len %u, code %d\n", millis(), offset, len, err);
    return false;
  }
  return true;
}
