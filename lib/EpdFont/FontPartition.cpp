#include "FontPartition.h"

#include <Logging.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

#include <cstring>

// ── On-flash structs (must match fontconvert.py v2 exactly) ─────────────────

static constexpr uint32_t PARTITION_MAGIC   = 0x43504654;  // "CPFT"
static constexpr uint32_t PARTITION_VERSION = 2;
static constexpr uint8_t  PARTITION_SUBTYPE = 0x40;

struct __attribute__((packed)) PartitionHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t fontCount;
  uint32_t dataSize;
  uint32_t groupCount;
};

struct __attribute__((packed)) GroupDirectoryEntry {
  char     name[16];
  uint32_t dataOffset;
  uint32_t dataSize;
};

struct __attribute__((packed)) FontDirectoryEntry {
  char     name[32];
  uint32_t bitmapOffset;
  uint32_t bitmapSize;
  uint32_t glyphOffset;
  uint32_t glyphCount;
  uint32_t intervalOffset;
  uint32_t intervalCount;
  uint8_t  advanceY;
  uint8_t  is2Bit;
  uint8_t  _pad[2];
  int32_t  ascender;
  int32_t  descender;
};

static_assert(sizeof(GroupDirectoryEntry) == 24, "GroupDirectoryEntry must be 24 bytes");
static_assert(sizeof(FontDirectoryEntry) == 68, "FontDirectoryEntry must be 68 bytes");

// ── Runtime state ───────────────────────────────────────────────────────────

static const esp_partition_t*   partition = nullptr;

// Directories read into RAM (small — a few KB)
static PartitionHeader          hdr;
static GroupDirectoryEntry*     groupDir = nullptr;
static FontDirectoryEntry*      fontDir  = nullptr;

// Per-font runtime data (pointers into mapped memory, or nullptr if unmapped)
static EpdFontData*             fontDataArray = nullptr;
static EpdFont**                fontArray = nullptr;
static int                      numFonts = 0;

// UI group mmap (always active)
static spi_flash_mmap_handle_t  uiMmapHandle = 0;
static const uint8_t*           uiBase = nullptr;
static int                      uiGroupIndex = -1;

// Reader group mmap (swappable)
static spi_flash_mmap_handle_t  readerMmapHandle = 0;
static const uint8_t*           readerBase = nullptr;
static int                      readerGroupIndex = -1;

// ── Helpers ─────────────────────────────────────────────────────────────────

static int findGroupIndex(const char* name) {
  for (uint32_t i = 0; i < hdr.groupCount; i++) {
    if (strncmp(groupDir[i].name, name, 16) == 0) return static_cast<int>(i);
  }
  return -1;
}

/// Determine which group a font belongs to by checking if its data offsets
/// fall within the group's data range.
static int fontGroupIndex(int fontIdx) {
  const uint32_t off = fontDir[fontIdx].bitmapOffset;
  for (uint32_t g = 0; g < hdr.groupCount; g++) {
    const uint32_t gStart = groupDir[g].dataOffset;
    const uint32_t gEnd   = gStart + groupDir[g].dataSize;
    if (off >= gStart && off < gEnd) return static_cast<int>(g);
  }
  return -1;
}

/// Build EpdFontData entries for all fonts in the given group, using the
/// provided mmap base pointer.
static void buildFontData(int groupIdx, const uint8_t* base) {
  const uint32_t gStart = groupDir[groupIdx].dataOffset;
  for (int i = 0; i < numFonts; i++) {
    if (fontGroupIndex(i) != groupIdx) continue;
    const auto& e = fontDir[i];
    fontDataArray[i] = {
        .bitmap        = base + (e.bitmapOffset - gStart),
        .glyph         = reinterpret_cast<const EpdGlyph*>(base + (e.glyphOffset - gStart)),
        .intervals     = reinterpret_cast<const EpdUnicodeInterval*>(base + (e.intervalOffset - gStart)),
        .intervalCount = e.intervalCount,
        .advanceY      = e.advanceY,
        .ascender      = e.ascender,
        .descender     = e.descender,
        .is2Bit        = e.is2Bit != 0,
    };
  }
}

/// Invalidate EpdFontData entries for all fonts in the given group.
static void clearFontData(int groupIdx) {
  for (int i = 0; i < numFonts; i++) {
    if (fontGroupIndex(i) != groupIdx) continue;
    memset(&fontDataArray[i], 0, sizeof(EpdFontData));
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

bool FontPartition::begin() {
  partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(PARTITION_SUBTYPE),
      "fontdata");

  if (!partition) {
    LOG_ERR("FONT", "fontdata partition not found");
    return false;
  }

  LOG_DBG("FONT", "fontdata partition: offset=0x%x size=0x%x", partition->address, partition->size);

  // Read header
  esp_err_t err = esp_partition_read(partition, 0, &hdr, sizeof(hdr));
  if (err != ESP_OK) {
    LOG_ERR("FONT", "Failed to read header: %s", esp_err_to_name(err));
    return false;
  }
  if (hdr.magic != PARTITION_MAGIC) {
    LOG_ERR("FONT", "Bad magic: 0x%08x (expected 0x%08x)", hdr.magic, PARTITION_MAGIC);
    return false;
  }
  if (hdr.version != PARTITION_VERSION) {
    LOG_ERR("FONT", "Unsupported version: %u (expected %u)", hdr.version, PARTITION_VERSION);
    return false;
  }

  numFonts = static_cast<int>(hdr.fontCount);

  // Read group directory into RAM
  const size_t groupDirBytes = hdr.groupCount * sizeof(GroupDirectoryEntry);
  groupDir = new GroupDirectoryEntry[hdr.groupCount];
  err = esp_partition_read(partition, sizeof(PartitionHeader), groupDir, groupDirBytes);
  if (err != ESP_OK) {
    LOG_ERR("FONT", "Failed to read group directory: %s", esp_err_to_name(err));
    return false;
  }

  // Read font directory into RAM
  const size_t fontDirBytes = numFonts * sizeof(FontDirectoryEntry);
  fontDir = new FontDirectoryEntry[numFonts];
  err = esp_partition_read(partition, sizeof(PartitionHeader) + groupDirBytes, fontDir, fontDirBytes);
  if (err != ESP_OK) {
    LOG_ERR("FONT", "Failed to read font directory: %s", esp_err_to_name(err));
    return false;
  }

  // Allocate EpdFontData array (zeroed — unmapped fonts start as null)
  fontDataArray = new EpdFontData[numFonts]();

  // Allocate stable EpdFont wrappers pointing into fontDataArray
  fontArray = new EpdFont*[numFonts];
  for (int i = 0; i < numFonts; i++) {
    fontArray[i] = new EpdFont(&fontDataArray[i]);
  }

  // Map the UI group
  uiGroupIndex = findGroupIndex("ui");
  if (uiGroupIndex < 0) {
    LOG_ERR("FONT", "No 'ui' group in partition");
    return false;
  }

  const auto& uiGroup = groupDir[uiGroupIndex];
  const void* ptr = nullptr;
  err = esp_partition_mmap(partition, uiGroup.dataOffset, uiGroup.dataSize,
                           SPI_FLASH_MMAP_DATA, &ptr, &uiMmapHandle);
  if (err != ESP_OK) {
    LOG_ERR("FONT", "Failed to mmap ui group: %s", esp_err_to_name(err));
    return false;
  }
  uiBase = static_cast<const uint8_t*>(ptr);
  buildFontData(uiGroupIndex, uiBase);

  LOG_DBG("FONT", "Loaded %d fonts, %u groups. UI mapped: %u KB",
          numFonts, hdr.groupCount, uiGroup.dataSize / 1024);
  return true;
}

bool FontPartition::loadReaderGroup(const char* groupName) {
  if (!partition || !fontDir) return false;

  const int idx = findGroupIndex(groupName);
  if (idx < 0) {
    LOG_ERR("FONT", "Reader group not found: %s", groupName);
    return false;
  }

  if (idx == readerGroupIndex) return true;

  // Unmap previous reader group
  if (readerMmapHandle) {
    clearFontData(readerGroupIndex);
    spi_flash_munmap(readerMmapHandle);
    readerMmapHandle = 0;
    readerBase = nullptr;
    readerGroupIndex = -1;
  }

  // Map new reader group
  const auto& group = groupDir[idx];
  const void* ptr = nullptr;
  esp_err_t err = esp_partition_mmap(partition, group.dataOffset, group.dataSize,
                                     SPI_FLASH_MMAP_DATA, &ptr, &readerMmapHandle);
  if (err != ESP_OK) {
    LOG_ERR("FONT", "Failed to mmap reader group '%s': %s", groupName, esp_err_to_name(err));
    return false;
  }

  readerBase = static_cast<const uint8_t*>(ptr);
  readerGroupIndex = idx;
  buildFontData(idx, readerBase);

  LOG_DBG("FONT", "Reader group '%s' mapped: %u KB", groupName, group.dataSize / 1024);
  return true;
}

const char* FontPartition::currentReaderGroup() {
  if (readerGroupIndex < 0) return nullptr;
  return groupDir[readerGroupIndex].name;
}

const EpdFontData* FontPartition::getFont(const char* name) {
  if (!fontDir || !fontDataArray) return nullptr;

  for (int i = 0; i < numFonts; i++) {
    if (strncmp(fontDir[i].name, name, 32) == 0) {
      if (fontDataArray[i].bitmap == nullptr) return nullptr;
      return &fontDataArray[i];
    }
  }

  LOG_ERR("FONT", "Font not found: %s", name);
  return nullptr;
}

EpdFont* FontPartition::getEpdFont(const char* name) {
  if (!fontDir || !fontArray) return nullptr;

  for (int i = 0; i < numFonts; i++) {
    if (strncmp(fontDir[i].name, name, 32) == 0) {
      return fontArray[i];
    }
  }

  LOG_ERR("FONT", "Font not found: %s", name);
  return nullptr;
}

int FontPartition::fontCount() { return numFonts; }
