#include "FlashFontPartition.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>

namespace FlashFontPartition {

// Partition magic
static constexpr char MAGIC[4] = {'C', 'P', 'F', 'C'};

// Sector / chunk sizes
static constexpr size_t SEC = 4096;    // flash erase granularity
static constexpr size_t CHUNK = 4096;  // SD read / flash write chunk

// --- Module state ---

static esp_partition_mmap_handle_t s_mmapHandle = 0;
static const uint8_t* s_mmapPtr = nullptr;  // base of mapped partition

// Write-session state (valid between beginWrite and finaliseWrite)
struct WriteSession {
  bool active = false;
  uint32_t nextDataOff = 0;  // next free byte offset for font data
  uint8_t entryCount = 0;
  Entry entries[MAX_ENTRIES] = {};
  size_t partitionSize = 0;
};
static WriteSession s_ws;

// --- Internal helpers ---

static const esp_partition_t* findPartition() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) LOG_ERR("FFP", "spiffs partition not found");
  return part;
}

// Ceiling for font data: the partition minus the language slot at its tail.
static size_t usableSizeOf(const esp_partition_t* part) {
  if (!part || part->size <= LANG_RESERVED_BYTES) return 0;
  return static_cast<size_t>(part->size) - LANG_RESERVED_BYTES;
}

size_t fontUsableSize() { return usableSizeOf(findPartition()); }

// Read the raw index from flash into a caller-supplied buffer.
// Returns true and fills `count` with the number of valid entries.
static bool readIndex(const esp_partition_t* part, Entry* entries, uint8_t& count) {
  uint8_t hdr[8];
  if (esp_partition_read(part, 0, hdr, 8) != ESP_OK) return false;
  if (memcmp(hdr, MAGIC, 4) != 0) return false;
  count = hdr[4];
  if (count == 0 || count > MAX_ENTRIES) return false;

  if (esp_partition_read(part, 8, entries, count * ENTRY_SIZE) != ESP_OK) return false;

  // Reject an index that reaches into the language slot. This only fires on a
  // partition written before the reservation existed; the cache is rebuilt from
  // the .cpfont files on the SD card, so nothing the user installed is lost.
  const size_t ceiling = usableSizeOf(part);
  for (uint8_t i = 0; i < count; i++) {
    const size_t end = static_cast<size_t>(entries[i].dataOffset) + entries[i].dataSize;
    if (end > ceiling) {
      LOG_INF("FFP", "index predates the language reservation (entry %u ends at %u > %u) — re-caching", i,
              static_cast<unsigned>(end), static_cast<unsigned>(ceiling));
      return false;
    }
  }
  return true;
}

// Find a matching entry by family + size. Returns -1 if not found.
static int findEntry(const Entry* entries, uint8_t count, const char* familyName, uint8_t pointSize) {
  for (uint8_t i = 0; i < count; i++) {
    if (entries[i].pointSize == pointSize && strncmp(entries[i].familyName, familyName, 31) == 0) return i;
  }
  return -1;
}

// Erase the partition up to `bytes` (rounded up to sector boundary).
static bool eraseRange(const esp_partition_t* part, size_t bytes) {
  const size_t eraseLen = (bytes + SEC - 1) & ~(SEC - 1);
  const size_t capped = std::min(eraseLen, static_cast<size_t>(part->size));
  return esp_partition_erase_range(part, 0, capped) == ESP_OK;
}

// --- Write API ---

bool beginWrite(const char* familyName) {
  (void)familyName;  // stored per-entry; not needed at begin time

  if (s_mmapPtr) {
    LOG_ERR("FFP", "beginWrite: partition is mmap'd — call unmap() first");
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;

  // Erase everything BELOW the language slot upfront so appendFile can write
  // sequentially. Erasing part->size here would take the language pack with it
  // on every font cache rebuild.
  const size_t usable = usableSizeOf(part);
  if (usable == 0) {
    LOG_ERR("FFP", "beginWrite: partition too small for the language reservation");
    s_ws.active = false;
    return false;
  }
  LOG_INF("FFP", "Erasing flash font partition (%u B of %u)...", static_cast<unsigned>(usable),
          static_cast<unsigned>(part->size));
  if (esp_partition_erase_range(part, 0, usable) != ESP_OK) {
    LOG_ERR("FFP", "beginWrite: erase failed");
    s_ws.active = false;  // ensure no stale session survives a failed erase
    return false;
  }

  s_ws.active = true;
  s_ws.entryCount = 0;
  s_ws.nextDataOff = static_cast<uint32_t>(HEADER_BYTES);
  // Lowering this is what makes appendFile's own overflow check respect the
  // reservation, including on the single-file path that skips PARTITION_CAP.
  s_ws.partitionSize = usable;
  memset(s_ws.entries, 0, sizeof(s_ws.entries));

  LOG_DBG("FFP", "beginWrite: partition ready, data starts at offset %u", static_cast<unsigned>(s_ws.nextDataOff));
  return true;
}

bool appendFile(const char* sdPath, const char* familyName, uint8_t pointSize) {
  if (!s_ws.active) {
    LOG_ERR("FFP", "appendFile: no active write session");
    return false;
  }
  if (s_ws.entryCount >= MAX_ENTRIES) {
    LOG_ERR("FFP", "appendFile: entry table full (%u entries)", MAX_ENTRIES);
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;

  HalFile file;
  if (!Storage.openFileForRead("FFP", sdPath, file) || !file) {
    LOG_ERR("FFP", "appendFile: failed to open %s", sdPath);
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize == 0) {
    LOG_ERR("FFP", "appendFile: empty file %s", sdPath);
    file.close();
    return false;
  }

  // 4-byte align the data offset
  const uint32_t dataOff = (s_ws.nextDataOff + 3u) & ~static_cast<uint32_t>(3u);

  if (dataOff + fileSize > s_ws.partitionSize) {
    LOG_ERR("FFP", "appendFile: %s (%u B) would overflow partition at offset %u", sdPath,
            static_cast<unsigned>(fileSize), static_cast<unsigned>(dataOff));
    file.close();
    return false;
  }

  LOG_INF("FFP", "appendFile: %s (%u B) → offset %u", sdPath, static_cast<unsigned>(fileSize),
          static_cast<unsigned>(dataOff));

  // Write the font data in CHUNK-sized pieces
  uint8_t buf[CHUNK];
  size_t pos = 0;
  while (pos < fileSize) {
    const size_t want = std::min<size_t>(CHUNK, fileSize - pos);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      LOG_ERR("FFP", "appendFile: SD read error @%u", static_cast<unsigned>(pos));
      file.close();
      return false;
    }
    if (esp_partition_write(part, dataOff + pos, buf, want) != ESP_OK) {
      LOG_ERR("FFP", "appendFile: flash write error @%u", static_cast<unsigned>(dataOff + pos));
      file.close();
      return false;
    }
    pos += want;
  }
  file.close();

  // Record the index entry
  Entry& e = s_ws.entries[s_ws.entryCount++];
  memset(&e, 0, sizeof(e));
  strncpy(e.familyName, familyName, sizeof(e.familyName) - 1);
  e.pointSize = pointSize;
  e.dataOffset = dataOff;
  e.dataSize = static_cast<uint32_t>(fileSize);

  s_ws.nextDataOff = dataOff + static_cast<uint32_t>(fileSize);
  LOG_DBG("FFP", "appendFile: entry[%u] %s@%u = %u B", s_ws.entryCount - 1, familyName, pointSize,
          static_cast<unsigned>(fileSize));
  return true;
}

bool finaliseWrite() {
  if (!s_ws.active) {
    LOG_ERR("FFP", "finaliseWrite: no active write session");
    return false;
  }
  if (s_ws.entryCount == 0) {
    LOG_ERR("FFP", "finaliseWrite: no entries appended");
    s_ws.active = false;
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) {
    s_ws.active = false;
    return false;
  }

  // Build the 8-byte header + packed entry table in one buffer.
  // HEADER_BYTES = 8 + MAX_ENTRIES * ENTRY_SIZE. Write the whole header
  // region in one shot; unused entry slots remain 0xFF from the erase.
  // We only serialise the actual entryCount entries; the rest are
  // flash-erased (0xFF) and will be ignored (count field limits iteration).
  uint8_t hdr[8] = {};
  memcpy(hdr, MAGIC, 4);
  hdr[4] = s_ws.entryCount;
  // hdr[5..7] = 0 (reserved)

  if (esp_partition_write(part, 0, hdr, 8) != ESP_OK) {
    LOG_ERR("FFP", "finaliseWrite: header write failed");
    s_ws.active = false;
    return false;
  }
  if (esp_partition_write(part, 8, s_ws.entries, s_ws.entryCount * ENTRY_SIZE) != ESP_OK) {
    LOG_ERR("FFP", "finaliseWrite: entry table write failed");
    s_ws.active = false;
    return false;
  }

  s_ws.active = false;
  LOG_INF("FFP", "finaliseWrite: %u entries committed, %u B used", s_ws.entryCount,
          static_cast<unsigned>(s_ws.nextDataOff));
  return true;
}

// --- Read API ---

bool mmap(const char* familyName, uint8_t pointSize, const uint8_t** outPtr, size_t* outSize) {
  if (s_mmapPtr) {
    LOG_ERR("FFP", "mmap: already mapped — call unmap() first");
    return false;
  }
  if (s_ws.active) {
    LOG_ERR("FFP", "mmap: write session still active");
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;

  Entry entries[MAX_ENTRIES];
  uint8_t count = 0;
  if (!readIndex(part, entries, count)) {
    LOG_ERR("FFP", "mmap: no valid index in partition");
    return false;
  }

  const int idx = findEntry(entries, count, familyName, pointSize);
  if (idx < 0) {
    LOG_DBG("FFP", "mmap: %s@%u not found in partition index", familyName, pointSize);
    return false;
  }

  const uint32_t dataOff = entries[idx].dataOffset;
  const uint32_t dataSize = entries[idx].dataSize;

  // Map this entry's own pages and nothing below it. The C3 has 128 MMU pages of
  // 64 KB shared by the app's code, its rodata and every mapping, and the app
  // leaves ~32 of them free. Mapping from the partition start needed the whole
  // family below the entry as well: a family's largest sizes (Bitter 18 pt: 41
  // pages) could not be mapped at all and loaded from SD instead.
  // esp_partition_mmap aligns the offset down to a page itself and returns a
  // pointer already advanced to `dataOff`.
  const size_t mapBytes =
      ((dataOff & 0xFFFFu) + static_cast<size_t>(dataSize) + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu);

  const void* rawPtr = nullptr;
  if (esp_partition_mmap(part, dataOff, dataSize, ESP_PARTITION_MMAP_DATA, &rawPtr, &s_mmapHandle) != ESP_OK) {
    LOG_ERR("FFP", "mmap: esp_partition_mmap failed (offset=%u, map=%u B)", static_cast<unsigned>(dataOff),
            static_cast<unsigned>(mapBytes));
    return false;
  }

  s_mmapPtr = static_cast<const uint8_t*>(rawPtr);

  *outPtr = s_mmapPtr;
  *outSize = dataSize;

  LOG_DBG("FFP", "mmap: %s@%u → ptr=%p size=%u (map=%u B)", familyName, pointSize, *outPtr,
          static_cast<unsigned>(dataSize), static_cast<unsigned>(mapBytes));
  return true;
}

void unmap() {
  if (!s_mmapPtr) return;
  esp_partition_munmap(s_mmapHandle);
  s_mmapHandle = 0;
  s_mmapPtr = nullptr;
  LOG_DBG("FFP", "unmap OK");
}

// --- Query API ---

bool hasValidIndex() {
  const esp_partition_t* part = findPartition();
  if (!part) return false;
  Entry entries[MAX_ENTRIES];
  uint8_t count = 0;
  return readIndex(part, entries, count);
}

bool hasEntry(const char* familyName, uint8_t pointSize) {
  const esp_partition_t* part = findPartition();
  if (!part) return false;
  Entry entries[MAX_ENTRIES];
  uint8_t count = 0;
  if (!readIndex(part, entries, count)) return false;
  return findEntry(entries, count, familyName, pointSize) >= 0;
}

bool hasFamilyComplete(const char* familyName, const uint8_t* sizes, uint8_t sizeCount) {
  const esp_partition_t* part = findPartition();
  if (!part) return false;
  Entry entries[MAX_ENTRIES];
  uint8_t count = 0;
  if (!readIndex(part, entries, count)) return false;
  for (uint8_t i = 0; i < sizeCount; i++) {
    if (findEntry(entries, count, familyName, sizes[i]) < 0) return false;
  }
  return true;
}

bool isMapped() { return s_mmapPtr != nullptr; }

}  // namespace FlashFontPartition
