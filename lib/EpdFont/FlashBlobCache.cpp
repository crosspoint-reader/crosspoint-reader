#include "FlashBlobCache.h"

#if defined(FLASH_BLOB_CACHE_EXTERNAL_BACKEND) || \
    (defined(ESP_PLATFORM) && !defined(SIMULATOR) && !defined(BOARD_HAS_PSRAM))
#define CROSSPOINT_FLASH_BLOBS 1
#else
#define CROSSPOINT_FLASH_BLOBS 0
#endif

#if CROSSPOINT_FLASH_BLOBS

#include <Logging.h>

#include <cstring>
#include <memory>
#include <new>

// Partition access. The device build maps these onto esp_partition; a host
// test can supply its own by defining FLASH_BLOB_CACHE_EXTERNAL_BACKEND.
namespace FlashBlobCache::backend {
bool open(uint32_t* size);
bool read(uint32_t offset, void* buf, uint32_t length);
bool erase(uint32_t offset, uint32_t length);
bool write(uint32_t offset, const void* buf, uint32_t length);
const uint8_t* map(uint32_t offset, uint32_t length, uint32_t* handle);
void unmap(uint32_t handle);
}  // namespace FlashBlobCache::backend

#ifndef FLASH_BLOB_CACHE_EXTERNAL_BACKEND
#include <esp_partition.h>

namespace FlashBlobCache::backend {
namespace {
const esp_partition_t* partition = nullptr;
}

bool open(uint32_t* size) {
  if (partition == nullptr) {
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  }
  if (partition == nullptr) return false;
  *size = partition->size;
  return true;
}
bool read(const uint32_t offset, void* buf, const uint32_t length) {
  return esp_partition_read(partition, offset, buf, length) == ESP_OK;
}
bool erase(const uint32_t offset, const uint32_t length) {
  return esp_partition_erase_range(partition, offset, length) == ESP_OK;
}
bool write(const uint32_t offset, const void* buf, const uint32_t length) {
  return esp_partition_write(partition, offset, buf, length) == ESP_OK;
}
const uint8_t* map(const uint32_t offset, const uint32_t length, uint32_t* handle) {
  const void* ptr = nullptr;
  esp_partition_mmap_handle_t h;
  if (esp_partition_mmap(partition, offset, length, ESP_PARTITION_MMAP_DATA, &ptr, &h) != ESP_OK) return nullptr;
  *handle = static_cast<uint32_t>(h);
  return static_cast<const uint8_t*>(ptr);
}
void unmap(const uint32_t handle) { esp_partition_munmap(static_cast<esp_partition_mmap_handle_t>(handle)); }
}  // namespace FlashBlobCache::backend
#endif

namespace FlashBlobCache {
namespace {

constexpr uint32_t kMagic = 0x32465043;  // "CPF2": 128 KB slots (a "CPFB" directory had 64 KB slots)
constexpr uint32_t kSlotSize = MAX_BLOB_BYTES;
constexpr uint32_t kSlots = 8;
constexpr uint32_t kDirSector = 4096;  // directory at offset 0; slot i at (i + 1) * kSlotSize
constexpr uint32_t kCopyChunk = 4096;

struct DirEntry {
  uint32_t magic;
  uint32_t key;
  uint32_t length;
  uint32_t sequence;  // write order; the lowest idle slot is recycled first
};

struct Mapping {
  const uint8_t* data;
  uint32_t handle;
  uint8_t slot;
};

Mapping gMappings[kSlots] = {};
uint8_t gMappingCount = 0;
int8_t gUsable = -1;  // -1 unchecked, 0 no, 1 yes

// Keys whose copy failed verification: the source does not hash to its key
// (a corrupt file, or a bad hash in its header), so copying it again would
// only erase and rewrite a slot on every face build. Oldest entry recycled.
uint32_t gRejected[kSlots] = {};
uint8_t gRejectedCount = 0;
uint8_t gRejectedNext = 0;

bool isRejected(const uint32_t key) {
  for (uint8_t i = 0; i < gRejectedCount; i++) {
    if (gRejected[i] == key) return true;
  }
  return false;
}

void rememberRejected(const uint32_t key) {
  gRejected[gRejectedNext] = key;
  gRejectedNext = static_cast<uint8_t>((gRejectedNext + 1) % kSlots);
  if (gRejectedCount < kSlots) gRejectedCount++;
}

uint32_t slotOffset(const uint32_t slot) { return (slot + 1) * kSlotSize; }

bool usable() {
  if (gUsable < 0) {
    uint32_t size = 0;
    gUsable = backend::open(&size) && size >= slotOffset(kSlots) ? 1 : 0;
    if (!gUsable) LOG_DBG("FBC", "No usable flash partition; layout fonts stay in RAM");
  }
  return gUsable == 1;
}

uint32_t contentKey(const uint8_t* data, const uint32_t length) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < length; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash | 1u;
}

bool slotMapped(const uint32_t slot) {
  for (uint8_t i = 0; i < gMappingCount; i++) {
    if (gMappings[i].slot == slot) return true;
  }
  return false;
}

// Maps a slot and checks it still holds the blob; unmaps on mismatch.
const uint8_t* mapVerified(const uint32_t slot, const uint32_t key, const uint32_t length) {
  if (gMappingCount >= kSlots) return nullptr;
  uint32_t handle = 0;
  const uint8_t* data = backend::map(slotOffset(slot), length, &handle);
  if (data == nullptr) return nullptr;
  if (contentKey(data, length) != key) {
    backend::unmap(handle);
    return nullptr;
  }
  gMappings[gMappingCount++] = Mapping{data, handle, static_cast<uint8_t>(slot)};
  return data;
}

bool copyIn(const uint32_t slot, const uint32_t length, const Reader read, void* ctx) {
  const std::unique_ptr<uint8_t[]> chunk(new (std::nothrow) uint8_t[kCopyChunk]);
  if (!chunk) {
    LOG_ERR("FBC", "OOM: %u-byte copy buffer", kCopyChunk);
    return false;
  }
  if (!backend::erase(slotOffset(slot), kSlotSize)) return false;
  for (uint32_t done = 0; done < length;) {
    const uint32_t n = length - done < kCopyChunk ? length - done : kCopyChunk;
    if (!read(ctx, done, chunk.get(), n) || !backend::write(slotOffset(slot) + done, chunk.get(), n)) return false;
    done += n;
  }
  return true;
}

}  // namespace

const uint8_t* acquire(const uint32_t key, const uint32_t length, const Reader read, void* ctx) {
  if (key == 0 || length == 0 || length > kSlotSize || isRejected(key) || !usable()) return nullptr;

  DirEntry dir[kSlots];
  if (!backend::read(0, dir, sizeof(dir))) return nullptr;

  uint32_t newest = 0;
  for (uint32_t slot = 0; slot < kSlots; slot++) {
    if (dir[slot].magic != kMagic) continue;
    if (dir[slot].sequence > newest) newest = dir[slot].sequence;
    if (dir[slot].key == key && dir[slot].length == length && !slotMapped(slot)) {
      if (const uint8_t* data = mapVerified(slot, key, length)) return data;
    }
  }

  // Not cached (or the copy went bad): recycle an empty slot, else the oldest idle one.
  int32_t victim = -1;
  for (uint32_t slot = 0; slot < kSlots; slot++) {
    if (slotMapped(slot)) continue;
    if (dir[slot].magic != kMagic) {
      victim = static_cast<int32_t>(slot);
      break;
    }
    if (victim < 0 || dir[slot].sequence < dir[victim].sequence) victim = static_cast<int32_t>(slot);
  }
  if (victim < 0) return nullptr;

  const auto slot = static_cast<uint32_t>(victim);
  LOG_DBG("FBC", "Copying %u-byte layout font to flash slot %u", length, slot);
  if (!copyIn(slot, length, read, ctx)) {
    LOG_ERR("FBC", "Flash copy to slot %u failed", slot);
    return nullptr;
  }
  const uint8_t* data = mapVerified(slot, key, length);
  if (data == nullptr) {
    LOG_ERR("FBC", "Slot %u failed verification after copy", slot);
    rememberRejected(key);
    return nullptr;
  }
  // The entry is recorded only after the data verified, so an interrupted
  // copy leaves the slot unlisted and it is simply rewritten next time.
  dir[slot] = DirEntry{kMagic, key, length, newest + 1};
  if (!backend::erase(0, kDirSector) || !backend::write(0, dir, sizeof(dir))) {
    LOG_ERR("FBC", "Flash directory update failed");
  }
  return data;
}

void release(void* data) {
  for (uint8_t i = 0; i < gMappingCount; i++) {
    if (gMappings[i].data != data) continue;
    backend::unmap(gMappings[i].handle);
    gMappings[i] = gMappings[--gMappingCount];
    return;
  }
}

}  // namespace FlashBlobCache

#else

namespace FlashBlobCache {
const uint8_t* acquire(uint32_t, uint32_t, Reader, void*) { return nullptr; }
void release(void*) {}
}  // namespace FlashBlobCache

#endif
