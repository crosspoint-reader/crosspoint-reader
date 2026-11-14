#include "SymbolTable.h"

#include <InflateReader.h>

#include <cstddef>
#include <cstring>
#include <new>

#include "esp_app_format.h"
#include "esp_ota_ops.h"

namespace SymbolTable {

namespace {

constexpr char MAGIC[4] = {'E', 'S', 'Y', 'M'};
constexpr uint16_t VERSION = 1;
// Refuses a damaged header before it sizes an allocation.
constexpr uint32_t MAX_BLOCK_SIZE = 16 * 1024;

struct Header {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t blockCount;
  uint32_t maxBlockSize;
};

struct IndexEntry {
  uint32_t address;
  uint32_t offset;
};

bool readIndex(const Table& table, uint32_t index, IndexEntry& entry) {
  return esp_partition_read(table.partition, table.offset + table.indexOffset + index * sizeof(IndexEntry), &entry,
                            sizeof(entry)) == ESP_OK;
}

// Feeds a block's deflate data from flash to uzlib in small pieces.
struct BlockSource {
  InflateReader reader;  // first member: the callback casts uzlib's pointer back to this
  const esp_partition_t* partition;
  uint32_t offset;
  uint32_t remaining;
  uint8_t buffer[128];

  static int refill(uzlib_uncomp* uncomp) {
    auto* self = reinterpret_cast<BlockSource*>(uncomp);
    const uint32_t length = self->remaining < sizeof(self->buffer) ? self->remaining : sizeof(self->buffer);
    if (length == 0 || esp_partition_read(self->partition, self->offset, self->buffer, length) != ESP_OK) {
      return -1;
    }
    self->offset += length;
    self->remaining -= length;
    uncomp->source = self->buffer + 1;
    uncomp->source_limit = self->buffer + length;
    return self->buffer[0];
  }
};
static_assert(offsetof(BlockSource, reader) == 0, "refill casts uzlib's pointer to BlockSource");

bool loadBlock(Table& table, uint32_t index) {
  if (table.loadedBlock == index) {
    return true;
  }
  table.loadedBlock = UINT32_MAX;

  IndexEntry entry;
  IndexEntry next;
  if (!readIndex(table, index, entry) || !readIndex(table, index + 1, next) || entry.offset >= next.offset ||
      next.offset > table.size) {
    return false;
  }

  BlockSource source;
  source.partition = table.partition;
  source.offset = table.offset + entry.offset;
  source.remaining = next.offset - entry.offset;
  source.reader.init();
  source.reader.setReadCallback(BlockSource::refill);
  size_t produced = 0;
  // Ok is valid too: the largest block fills the buffer before uzlib reads the end of its stream.
  if (source.reader.readAtMost(table.block.get(), table.maxBlockSize, &produced) == InflateStatus::Error) {
    return false;
  }

  table.loadedBlock = index;
  table.blockAddress = entry.address;
  table.blockEndAddress = next.address;
  table.blockSize = produced;
  return true;
}

bool readVarint(const uint8_t*& pos, const uint8_t* end, uint32_t& value) {
  value = 0;
  for (int shift = 0; shift < 32 && pos < end; shift += 7) {
    const uint8_t byte = *pos++;
    value |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if (byte < 0x80) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool open(Table& table) {
  table = Table{};
  const esp_partition_t* partition = esp_ota_get_running_partition();
  esp_image_header_t image;
  if (!partition || esp_partition_read(partition, 0, &image, sizeof(image)) != ESP_OK ||
      image.magic != ESP_IMAGE_HEADER_MAGIC || image.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
    return false;
  }

  // The table is the segment with load address 0 that starts with the magic.
  uint32_t offset = sizeof(image);
  for (uint8_t i = 0; i < image.segment_count; i++) {
    esp_image_segment_header_t segment;
    if (offset > partition->size - sizeof(segment) ||
        esp_partition_read(partition, offset, &segment, sizeof(segment)) != ESP_OK) {
      return false;
    }
    offset += sizeof(segment);
    if (segment.data_len > partition->size - offset) {
      return false;
    }

    Header header;
    if (segment.load_addr != 0 || segment.data_len < sizeof(header) ||
        esp_partition_read(partition, offset, &header, sizeof(header)) != ESP_OK ||
        memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0) {
      offset += segment.data_len;
      continue;
    }

    if (header.version != VERSION || header.headerSize < sizeof(header) || header.headerSize > segment.data_len ||
        header.blockCount == 0 || header.blockCount >= (segment.data_len - header.headerSize) / sizeof(IndexEntry) ||
        header.maxBlockSize == 0 || header.maxBlockSize > MAX_BLOCK_SIZE) {
      return false;
    }
    table.partition = partition;
    table.offset = offset;
    table.size = segment.data_len;
    table.indexOffset = header.headerSize;
    table.blockCount = header.blockCount;
    table.maxBlockSize = header.maxBlockSize;

    IndexEntry first;
    IndexEntry last;
    if (!readIndex(table, 0, first) || !readIndex(table, table.blockCount, last)) {
      table = Table{};
      return false;
    }
    table.firstAddress = first.address;
    table.endAddress = last.address;

    table.block.reset(new (std::nothrow) uint8_t[table.maxBlockSize]);
    if (!table.block) {
      table = Table{};
      return false;
    }
    return true;
  }
  return false;
}

bool lookup(Table& table, const uint32_t address, Function& function) {
  if (!table.block || address < table.firstAddress || address >= table.endAddress) {
    return false;
  }

  // Last block that starts at or below the address.
  uint32_t low = 0;
  uint32_t high = table.blockCount;
  while (high - low > 1) {
    const uint32_t mid = low + (high - low) / 2;
    IndexEntry entry;
    if (!readIndex(table, mid, entry)) {
      return false;
    }
    if (entry.address <= address) {
      low = mid;
    } else {
      high = mid;
    }
  }
  if (!loadBlock(table, low)) {
    return false;
  }

  // An entry runs to the start of the next one, or to the end of the block.
  const uint8_t* pos = table.block.get();
  const uint8_t* const end = pos + table.blockSize;
  const char* name = nullptr;
  uint32_t start = table.blockAddress;
  uint32_t next = table.blockAddress;
  while (pos < end) {
    uint32_t delta;
    if (!readVarint(pos, end, delta)) {
      return false;
    }
    const auto* nameEnd = static_cast<const uint8_t*>(memchr(pos, '\0', end - pos));
    if (!nameEnd) {
      return false;
    }
    next += delta;
    if (address < next) {
      break;
    }
    start = next;
    name = reinterpret_cast<const char*>(pos);
    pos = nameEnd + 1;
  }
  if (pos >= end) {
    next = table.blockEndAddress;
  }
  // An empty name marks a hole between functions.
  if (!name || name[0] == '\0' || address >= next) {
    return false;
  }
  function.name = name;
  function.offset = address - start;
  function.size = next - start;
  function.isLibraryRange = name[0] == '[';
  return true;
}

}  // namespace SymbolTable
