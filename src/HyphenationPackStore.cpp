#include "HyphenationPackStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace HyphenationPackStore {
namespace {

constexpr uint32_t BANK_MAGIC = 0x4B505943;  // CYPK
constexpr uint16_t BANK_VERSION = 1;
constexpr uint8_t PACK_VERSION = 1;
constexpr size_t SECTOR_SIZE = 4096;
constexpr size_t MAX_PACKS = 48;
constexpr size_t COPY_BUFFER_SIZE = 1024;

struct PackFileHeader {
  char magic[4];
  uint8_t version;
  char code[2];
  uint8_t minPrefix;
  uint8_t minSuffix;
  uint8_t flags;
  uint16_t reserved;
  uint32_t rootOffset;
  uint32_t payloadSize;
  uint32_t crc32;
};
static_assert(sizeof(PackFileHeader) == 24);
static_assert(offsetof(PackFileHeader, rootOffset) == 12);

struct BankEntry {
  char code[2];
  uint8_t minPrefix;
  uint8_t minSuffix;
  uint32_t offset;
  uint32_t size;
  uint32_t rootOffset;
  uint32_t crc32;
};
static_assert(sizeof(BankEntry) == 20);

struct BankHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t generation;
  BankEntry entries[MAX_PACKS];
  uint32_t crc32;
};
static_assert(sizeof(BankHeader) == 976 && sizeof(BankHeader) <= SECTOR_SIZE);

const esp_partition_t* partition = nullptr;
size_t bankSize = 0;
bool storeReady = false;
int activeBank = -1;
const uint8_t* mappedBank = nullptr;
esp_partition_mmap_handle_t mapHandle = 0;
BankHeader activeHeader{};
BankHeader nextHeader{};

size_t bankOffset(const int bank) { return static_cast<size_t>(bank) * bankSize; }
size_t align4(const size_t value) { return (value + 3u) & ~size_t{3u}; }

uint32_t headerCrc(const BankHeader& header) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&header), offsetof(BankHeader, crc32));
}

const BankEntry* findEntry(const BankHeader& header, const char* code) {
  for (size_t i = 0; i < header.count; ++i) {
    if (header.entries[i].code[0] == code[0] && header.entries[i].code[1] == code[1]) {
      return &header.entries[i];
    }
  }
  return nullptr;
}

bool validHeader(const BankHeader& header) {
  if (header.magic != BANK_MAGIC || header.version != BANK_VERSION || header.count > MAX_PACKS ||
      header.crc32 != headerCrc(header)) {
    return false;
  }
  size_t end = SECTOR_SIZE;
  for (size_t i = 0; i < header.count; ++i) {
    const auto& entry = header.entries[i];
    if (entry.code[0] < 'a' || entry.code[0] > 'z' || entry.code[1] < 'a' || entry.code[1] > 'z' ||
        entry.offset < end || entry.size == 0 || entry.rootOffset >= entry.size ||
        static_cast<size_t>(entry.offset) + entry.size > bankSize) {
      return false;
    }
    end = align4(static_cast<size_t>(entry.offset) + entry.size);
  }
  return true;
}

bool readHeader(const int bank, BankHeader& header) {
  return esp_partition_read(partition, bankOffset(bank), &header, sizeof(header)) == ESP_OK && validHeader(header);
}

size_t usedBytes(const BankHeader& header) {
  if (header.count == 0) return SECTOR_SIZE;
  const auto& last = header.entries[header.count - 1];
  return align4(static_cast<size_t>(last.offset) + last.size);
}

bool mapActive() {
  if (activeHeader.count == 0) return true;
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle = 0;
  const esp_err_t err = esp_partition_mmap(partition, bankOffset(activeBank), usedBytes(activeHeader),
                                           ESP_PARTITION_MMAP_DATA, &mapped, &handle);
  if (err != ESP_OK) {
    LOG_ERR("HYPH", "Flash mapping failed: %d", err);
    return false;
  }
  mappedBank = static_cast<const uint8_t*>(mapped);
  mapHandle = handle;
  return true;
}

bool copyPartitionPayload(const BankEntry& source, const size_t destination, uint8_t* buffer) {
  size_t copied = 0;
  while (copied < source.size) {
    const size_t length = std::min(COPY_BUFFER_SIZE, static_cast<size_t>(source.size) - copied);
    if (esp_partition_read(partition, bankOffset(activeBank) + source.offset + copied, buffer, length) != ESP_OK ||
        esp_partition_write(partition, destination + copied, buffer, length) != ESP_OK) {
      LOG_ERR("HYPH", "Flash copy failed for %c%c", source.code[0], source.code[1]);
      return false;
    }
    copied += length;
    if ((copied % SECTOR_SIZE) == 0) delay(1);
  }
  return true;
}

bool copySdPayload(HalFile& source, const size_t destination, const uint32_t size, uint8_t* buffer) {
  if (!source.seek(sizeof(PackFileHeader))) return false;
  size_t copied = 0;
  while (copied < size) {
    const size_t length = std::min(COPY_BUFFER_SIZE, static_cast<size_t>(size) - copied);
    if (source.read(buffer, length) != static_cast<int>(length) ||
        esp_partition_write(partition, destination + copied, buffer, length) != ESP_OK) {
      LOG_ERR("HYPH", "SD-to-flash copy failed");
      return false;
    }
    copied += length;
    if ((copied % SECTOR_SIZE) == 0) delay(1);
  }
  return true;
}

bool rewriteBank(const char* replacedCode, HalFile* newFile, const PackFileHeader* pack, uint8_t* buffer) {
  nextHeader = {};
  nextHeader.magic = BANK_MAGIC;
  nextHeader.version = BANK_VERSION;
  nextHeader.generation = activeBank >= 0 ? activeHeader.generation + 1 : 1;

  size_t nextOffset = SECTOR_SIZE;
  for (size_t i = 0; i < activeHeader.count; ++i) {
    const auto& existing = activeHeader.entries[i];
    if (existing.code[0] == replacedCode[0] && existing.code[1] == replacedCode[1]) continue;
    BankEntry& entry = nextHeader.entries[nextHeader.count++];
    entry = existing;
    entry.offset = nextOffset;
    nextOffset = align4(nextOffset + entry.size);
  }
  if (pack) {
    if (nextHeader.count >= MAX_PACKS) return false;
    BankEntry& entry = nextHeader.entries[nextHeader.count++];
    entry.code[0] = pack->code[0];
    entry.code[1] = pack->code[1];
    entry.minPrefix = pack->minPrefix;
    entry.minSuffix = pack->minSuffix;
    entry.offset = nextOffset;
    entry.size = pack->payloadSize;
    entry.rootOffset = pack->rootOffset;
    entry.crc32 = pack->crc32;
    nextOffset = align4(nextOffset + entry.size);
  }
  if (nextOffset > bankSize) {
    LOG_ERR("HYPH", "No room for %zu bytes of packs", nextOffset);
    return false;
  }

  const int targetBank = activeBank == 0 ? 1 : 0;
  const size_t targetBase = bankOffset(targetBank);
  for (size_t offset = 0; offset < nextOffset; offset += SECTOR_SIZE) {
    if (esp_partition_erase_range(partition, targetBase + offset, SECTOR_SIZE) != ESP_OK) {
      LOG_ERR("HYPH", "Flash erase failed at %zu", offset);
      return false;
    }
    delay(1);
  }

  for (size_t i = 0; i < nextHeader.count; ++i) {
    const auto& destination = nextHeader.entries[i];
    const size_t flashOffset = targetBase + destination.offset;
    if (pack && destination.code[0] == pack->code[0] && destination.code[1] == pack->code[1]) {
      if (!copySdPayload(*newFile, flashOffset, pack->payloadSize, buffer)) return false;
    } else {
      const char code[] = {destination.code[0], destination.code[1], '\0'};
      const BankEntry* source = findEntry(activeHeader, code);
      if (!source || !copyPartitionPayload(*source, flashOffset, buffer)) return false;
    }
  }

  nextHeader.crc32 = headerCrc(nextHeader);
  if (esp_partition_write(partition, targetBase, &nextHeader, sizeof(nextHeader)) != ESP_OK) {
    LOG_ERR("HYPH", "Flash commit failed");
    return false;
  }

  const void* nextMapping = nullptr;
  esp_partition_mmap_handle_t nextHandle = 0;
  if (nextHeader.count > 0 && esp_partition_mmap(partition, targetBase, usedBytes(nextHeader), ESP_PARTITION_MMAP_DATA,
                                                 &nextMapping, &nextHandle) != ESP_OK) {
    LOG_ERR("HYPH", "Could not map committed bank");
    return false;
  }

  if (mappedBank) {
    esp_partition_munmap(mapHandle);
  }
  mappedBank = static_cast<const uint8_t*>(nextMapping);
  mapHandle = nextHandle;
  activeBank = targetBank;
  activeHeader = nextHeader;
  return true;
}

}  // namespace

bool begin() {
  storeReady = false;
  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition || partition->size < 2 * SECTOR_SIZE) {
    LOG_ERR("HYPH", "Internal flash partition unavailable");
    return false;
  }
  bankSize = (partition->size / 2) & ~(SECTOR_SIZE - 1);
  const bool firstValid = readHeader(0, activeHeader);
  const bool secondValid = readHeader(1, nextHeader);
  if (secondValid && (!firstValid || static_cast<int32_t>(nextHeader.generation - activeHeader.generation) > 0)) {
    activeHeader = nextHeader;
    activeBank = 1;
  } else if (firstValid) {
    activeBank = 0;
  } else {
    activeHeader = {};
    activeBank = -1;
  }
  if (activeBank < 0 || mapActive()) {
    storeReady = true;
    return true;
  }
  activeBank = -1;
  activeHeader = {};
  return false;
}

bool lookup(const char* primaryTag, ExternalHyphenationPatterns& out) {
  if (!storeReady || !primaryTag || !mappedBank) return false;
  const BankEntry* entry = findEntry(activeHeader, primaryTag);
  if (!entry) return false;
  out.patterns = {entry->rootOffset, mappedBank + entry->offset, entry->size};
  out.identity = entry->crc32;
  return true;
}

bool isInstalled(const char* primaryTag) {
  return storeReady && primaryTag && activeBank >= 0 && findEntry(activeHeader, primaryTag) != nullptr;
}

size_t installedCount() { return storeReady && activeBank >= 0 ? activeHeader.count : 0; }

bool installedAt(const size_t index, char code[3], uint32_t& size) {
  if (!storeReady || activeBank < 0 || index >= activeHeader.count) return false;
  const auto& entry = activeHeader.entries[index];
  code[0] = entry.code[0];
  code[1] = entry.code[1];
  code[2] = '\0';
  size = entry.size;
  return true;
}

InstallResult installFromSd(const char* path) {
  if (!storeReady) return InstallResult::FLASH_ERROR;
  HalFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return InstallResult::IO_ERROR;

  PackFileHeader pack{};
  if (file.read(&pack, sizeof(pack)) != sizeof(pack) || std::memcmp(pack.magic, "CPHY", 4) != 0 ||
      pack.version != PACK_VERSION || pack.reserved != 0 || pack.flags != 0 || pack.payloadSize == 0 ||
      pack.rootOffset >= pack.payloadSize || pack.payloadSize > bankSize - SECTOR_SIZE ||
      file.size() != sizeof(pack) + pack.payloadSize) {
    return InstallResult::INVALID_FILE;
  }
  const char code[] = {pack.code[0], pack.code[1], '\0'};
  const LanguageEntry* language = findLanguageEntry(code);
  if (!language || language->hyphenator) return InstallResult::UNSUPPORTED;
  if (pack.minPrefix != language->minPrefix || pack.minSuffix != language->minSuffix) {
    return InstallResult::INVALID_FILE;
  }

  // One temporary heap buffer avoids a large task-stack object and is reused
  // for validation and both flash-copy paths.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("HYPH", "OOM: %zu byte copy buffer", COPY_BUFFER_SIZE);
    return InstallResult::IO_ERROR;
  }
  uint32_t crc = 0;
  size_t remaining = pack.payloadSize;
  while (remaining > 0) {
    const size_t length = std::min(COPY_BUFFER_SIZE, remaining);
    if (file.read(buffer.get(), length) != static_cast<int>(length)) return InstallResult::IO_ERROR;
    crc = esp_rom_crc32_le(crc, buffer.get(), length);
    remaining -= length;
  }
  if (crc != pack.crc32) return InstallResult::INVALID_FILE;

  const BankEntry* current = activeBank >= 0 ? findEntry(activeHeader, code) : nullptr;
  if (current && current->crc32 == pack.crc32 && current->size == pack.payloadSize) return InstallResult::OK;
  size_t total = SECTOR_SIZE + align4(pack.payloadSize);
  for (size_t i = 0; i < activeHeader.count; ++i) {
    const auto& entry = activeHeader.entries[i];
    if (entry.code[0] != code[0] || entry.code[1] != code[1]) total += align4(entry.size);
  }
  if (total > bankSize) return InstallResult::NO_SPACE;
  return rewriteBank(code, &file, &pack, buffer.get()) ? InstallResult::OK : InstallResult::FLASH_ERROR;
}

bool remove(const char* primaryTag) {
  if (!storeReady || !primaryTag || !isInstalled(primaryTag)) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("HYPH", "OOM: %zu byte copy buffer", COPY_BUFFER_SIZE);
    return false;
  }
  return rewriteBank(primaryTag, nullptr, nullptr, buffer.get());
}

}  // namespace HyphenationPackStore
