#include "FlashLangPartition.h"

#include <InflateReader.h>
#include <Logging.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FlashFontPartition.h"

namespace FlashLangPartition {

static constexpr char MAGIC[4] = {'C', 'P', 'L', 'P'};
static constexpr size_t WRITE_CHUNK = 512;
// esp_partition_erase_range() works in whole sectors, esp_partition_mmap() in whole MMU pages.
static constexpr size_t SECTOR_BYTES = 4096;
static constexpr size_t MAP_PAGE_BYTES = 64 * 1024;

static esp_partition_mmap_handle_t s_mmapHandle = 0;
static const uint8_t* s_mmapPtr = nullptr;
static size_t s_mapBytes = 0;  // how much of the slot the live mapping covers

static constexpr size_t roundUp(const size_t v, const size_t to) { return (v + to - 1) / to * to; }

// CRC-32 as zlib computes it, chained from 0. ROM code: costs no flash.
static uint32_t crc32(const uint32_t crc, const void* data, const size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t*>(data), static_cast<uint32_t>(len));
}

// On-flash header, read and written as a unit. Packed layout is fixed by the
// comment in the header file; the struct mirrors it so the offsets stay in one
// place rather than being recomputed at each access.
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t langIndex;
  uint16_t keyCount;
  uint32_t stamp;
  uint32_t tableOff;
  uint32_t dataOff;
  uint32_t dataLen;
  uint32_t extOff;  // 0 with extLen 0: no extension
  uint32_t extLen;
  uint32_t stringsCrc;  // the data, then the table
  uint32_t extCrc;
};
static_assert(sizeof(Header) == HEADER_BYTES, "language slot header layout changed");

static const esp_partition_t* findPartition() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) LOG_ERR("FLP", "spiffs partition not found");
  return part;
}

// Byte offset of the language slot within the partition. Equal to
// FlashFontPartition's ceiling, and 64 KB aligned because the partition both
// starts on and is a multiple of 64 KB — esp_partition_mmap requires that.
static size_t slotOffset(const esp_partition_t* part) {
  if (!part || part->size <= FlashFontPartition::LANG_RESERVED_BYTES) return 0;
  return static_cast<size_t>(part->size) - FlashFontPartition::LANG_RESERVED_BYTES;
}

static bool readHeader(const esp_partition_t* part, Header& out) {
  const size_t off = slotOffset(part);
  if (off == 0) return false;
  if (esp_partition_read(part, off, &out, sizeof(Header)) != ESP_OK) return false;
  if (memcmp(out.magic, MAGIC, 4) != 0) return false;
  if (out.version != FORMAT_VERSION) return false;
  return true;
}

// The table and the strings where build() puts them, inside the slot. A header that says otherwise is
// not one this firmware wrote.
static bool layoutValid(const Header& h) {
  return h.tableOff == HEADER_BYTES && h.dataOff >= h.tableOff + static_cast<uint32_t>(h.keyCount) * 2u &&
         static_cast<size_t>(h.dataOff) + h.dataLen <= FlashFontPartition::LANG_RESERVED_BYTES;
}

// The extension a header describes, if it is one this firmware can trust: after the strings and
// inside the slot. Anything else reads as none.
static bool extensionValid(const Header& h) {
  return h.extLen != 0 && h.extOff >= h.dataOff + h.dataLen && h.extOff % 4 == 0 &&
         static_cast<size_t>(h.extOff) + h.extLen <= FlashFontPartition::LANG_RESERVED_BYTES;
}

bool holds(const uint8_t langIndex, const uint32_t stamp, const uint32_t extBytes) {
  const esp_partition_t* part = findPartition();
  if (!part) return false;
  Header h;
  if (!readHeader(part, h)) return false;
  const uint32_t ext = extensionValid(h) ? h.extLen : 0;
  return h.langIndex == langIndex && h.stamp == stamp && ext == extBytes;
}

// Buffers an extension's bytes and programs them in WRITE_CHUNK pieces from `base` on.
class ChunkedSlotWriter final : public SlotWriter {
 public:
  ChunkedSlotWriter(const esp_partition_t* part, const size_t base, const size_t limit, uint8_t* chunk,
                    uint8_t* scratch, const size_t scratchBytes)
      : part_(part), base_(base), limit_(limit), chunk_(chunk), scratch_(scratch), scratchBytes_(scratchBytes) {}

  bool write(const void* data, size_t len) override {
    if (failed_ || written_ + fill_ + len > limit_) {
      failed_ = true;
      return false;
    }
    crc_ = crc32(crc_, data, len);
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (len > 0) {
      const size_t take = std::min(len, WRITE_CHUNK - fill_);
      memcpy(chunk_ + fill_, bytes, take);
      fill_ += take;
      bytes += take;
      len -= take;
      if (fill_ == WRITE_CHUNK && !flush()) return false;
    }
    return true;
  }
  uint8_t* scratch() override { return scratch_; }
  size_t scratchBytes() const override { return scratchBytes_; }

  // Programs what is still buffered. The total written, once this succeeds.
  bool finish() { return flush(); }
  size_t written() const { return written_; }
  // CRC32 of every byte handed to write(): what map() expects to read back.
  uint32_t crc() const { return crc_; }

 private:
  bool flush() {
    if (failed_) return false;
    if (fill_ == 0) return true;
    if (esp_partition_write(part_, base_ + written_, chunk_, fill_) != ESP_OK) {
      LOG_ERR("FLP", "extension write failed at %u B", static_cast<unsigned>(written_));
      failed_ = true;
      return false;
    }
    written_ += fill_;
    fill_ = 0;
    return true;
  }

  const esp_partition_t* part_;
  size_t base_;
  size_t limit_;
  uint8_t* chunk_;
  uint8_t* scratch_;
  size_t scratchBytes_;
  size_t fill_ = 0;
  size_t written_ = 0;
  uint32_t crc_ = 0;
  bool failed_ = false;
};

// Walks the decompressed blob one byte at a time, filling the offset table.
//
// Kept as explicit state rather than scanning the finished blob because the
// blob is streamed to flash in chunks and never exists in RAM as a whole. The
// state lives in members, so an entry straddling a chunk boundary needs no
// special case at all.
class TableBuilder {
 public:
  TableBuilder(uint16_t* table, uint16_t keyCount, const uint16_t* enOffsets)
      : table_(table), keyCount_(keyCount), enOffsets_(enOffsets) {}

  void feed(const uint8_t* bytes, const size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (atEntryStart_ && entry_ < keyCount_) {
        // An empty entry (immediate NUL) means "no override" — point the table
        // at English instead, exactly as the generator does for in-image
        // languages, so I18n::get() needs no second kind of fallback.
        table_[entry_] =
            (bytes[i] == 0) ? static_cast<uint16_t>(0x8000u | enOffsets_[entry_]) : static_cast<uint16_t>(pos_);
        atEntryStart_ = false;
      }
      if (bytes[i] == 0) {
        entry_++;
        atEntryStart_ = true;
      }
      pos_++;
    }
  }

  bool complete() const { return entry_ == keyCount_; }
  uint16_t entriesSeen() const { return entry_; }

 private:
  uint16_t* table_;
  uint16_t keyCount_;
  const uint16_t* enOffsets_;
  uint32_t pos_ = 0;
  uint16_t entry_ = 0;
  bool atEntryStart_ = true;
};

bool build(const uint8_t langIndex, const uint32_t stamp, const uint8_t* compressed, const size_t compressedLen,
           const size_t uncompressedLen, const uint16_t keyCount, const uint16_t* enOffsets, const Extension* ext) {
  if (!compressed || compressedLen == 0 || keyCount == 0 || !enOffsets) return false;

  // TableBuilder narrows blob positions to uint16_t, and bit 15 is the English
  // fallback flag, so a blob at or past 32 KB would alias offsets onto that
  // flag. gen_i18n.py refuses to emit one (MAX_BLOB_BYTES) — this is the
  // matching check on the consuming side, so the two cannot drift apart
  // silently if the generator is ever changed.
  if (uncompressedLen >= 0x8000u) {
    LOG_ERR("FLP", "language %u blob is %u B, past the 15-bit offset limit", langIndex,
            static_cast<unsigned>(uncompressedLen));
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;
  const size_t slot = slotOffset(part);
  if (slot == 0) {
    LOG_ERR("FLP", "partition too small for a language slot");
    return false;
  }

  // A live mapping would go stale under the erase below.
  unmap();

  // 4-byte align the table length so the data that follows starts aligned too,
  // matching FlashFontPartition's own convention for flash writes. An odd key
  // count would otherwise leave every data write 2-byte aligned.
  const size_t tableOff = HEADER_BYTES;
  const size_t tableBytes = (static_cast<size_t>(keyCount) * sizeof(uint16_t) + 3u) & ~static_cast<size_t>(3u);
  const size_t tableWords = tableBytes / sizeof(uint16_t);
  const size_t dataOff = tableOff + tableBytes;
  if (dataOff + uncompressedLen > FlashFontPartition::LANG_RESERVED_BYTES) {
    LOG_ERR("FLP", "language %u needs %u B, slot is %u B", langIndex, static_cast<unsigned>(dataOff + uncompressedLen),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES));
    return false;
  }

  // The extension goes after the strings, 4-byte aligned for the tables in it. One that does not fit
  // is dropped here rather than failing the strings.
  const size_t extOff = (dataOff + uncompressedLen + 3u) & ~static_cast<size_t>(3u);
  if (ext && ext->bytes > 0 && extOff + ext->bytes > FlashFontPartition::LANG_RESERVED_BYTES) {
    LOG_ERR("FLP", "language %u extension needs %u B past %u B, slot is %u B - building the strings alone", langIndex,
            static_cast<unsigned>(ext->bytes), static_cast<unsigned>(extOff),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES));
    ext = nullptr;
  }
  if (ext && ext->bytes == 0) ext = nullptr;

  // Only the sectors this build writes: the slot is twice what a pack without an extension needs.
  // Bytes past them may hold an earlier, longer build; the header bounds every read, so they are
  // never looked at.
  const size_t used = ext ? extOff + ext->bytes : dataOff + uncompressedLen;
  if (esp_partition_erase_range(part, slot, roundUp(used, SECTOR_BYTES)) != ESP_OK) {
    LOG_ERR("FLP", "erase failed");
    return false;
  }

  // All three buffers are transient — freed before this function returns.
  auto ring = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[INFLATE_RING_BYTES]);
  auto chunk = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[WRITE_CHUNK]);
  // Value-initialised: the alignment pad past keyCount must be written as a
  // known value, not whatever the heap held.
  auto table = std::unique_ptr<uint16_t[]>(new (std::nothrow) uint16_t[tableWords]());
  if (!ring || !chunk || !table) {
    LOG_ERR("FLP", "out of memory building language %u", langIndex);
    return false;
  }

  InflateReader inflate;
  if (!inflate.initWithExternalRing(ring.get(), INFLATE_RING_BYTES)) {
    LOG_ERR("FLP", "inflate init failed");
    return false;
  }
  inflate.setSource(compressed, compressedLen);

  TableBuilder builder(table.get(), keyCount, enOffsets);
  size_t written = 0;
  // Over the bytes as they are written, from RAM: a byte that did not program, or that something else
  // overwrites later, then fails map()'s check of what flash holds.
  uint32_t stringsCrc = 0;
  bool done = false;
  while (!done) {
    size_t produced = 0;
    const InflateStatus st = inflate.readAtMost(chunk.get(), WRITE_CHUNK, &produced);
    if (st == InflateStatus::Error) {
      LOG_ERR("FLP", "inflate failed at %u B", static_cast<unsigned>(written));
      return false;
    }
    if (produced > 0) {
      if (written + produced > uncompressedLen) {
        LOG_ERR("FLP", "stream longer than declared (%u B)", static_cast<unsigned>(uncompressedLen));
        return false;
      }
      if (esp_partition_write(part, slot + dataOff + written, chunk.get(), produced) != ESP_OK) {
        LOG_ERR("FLP", "write failed at %u B", static_cast<unsigned>(written));
        return false;
      }
      builder.feed(chunk.get(), produced);
      stringsCrc = crc32(stringsCrc, chunk.get(), produced);
      written += produced;
    }
    done = (st == InflateStatus::Done);
    if (!done && produced == 0) {
      LOG_ERR("FLP", "inflate stalled at %u B", static_cast<unsigned>(written));
      return false;
    }
  }

  if (written != uncompressedLen || !builder.complete()) {
    LOG_ERR("FLP", "blob mismatch: %u/%u B, %u/%u entries", static_cast<unsigned>(written),
            static_cast<unsigned>(uncompressedLen), builder.entriesSeen(), keyCount);
    return false;
  }

  if (esp_partition_write(part, slot + tableOff, table.get(), tableBytes) != ESP_OK) {
    LOG_ERR("FLP", "table write failed");
    return false;
  }
  stringsCrc = crc32(stringsCrc, table.get(), tableBytes);

  // The extension, through the staging chunk; the inflate ring is free now and is its scratch.
  uint32_t extLen = 0;
  uint32_t extCrc = 0;
  if (ext) {
    ChunkedSlotWriter writer(part, slot + extOff, ext->bytes, chunk.get(), ring.get(), INFLATE_RING_BYTES);
    if (ext->produce(ext->ctx, writer) && writer.finish() && writer.written() == ext->bytes) {
      extLen = ext->bytes;
      extCrc = writer.crc();
    } else {
      LOG_ERR("FLP", "language %u extension failed after %u of %u B - the strings stand alone", langIndex,
              static_cast<unsigned>(writer.written()), static_cast<unsigned>(ext->bytes));
    }
  }

  // Header last: until this lands the slot reads as absent.
  Header h = {};
  memcpy(h.magic, MAGIC, 4);
  h.version = FORMAT_VERSION;
  h.langIndex = langIndex;
  h.keyCount = keyCount;
  h.stamp = stamp;
  h.tableOff = static_cast<uint32_t>(tableOff);
  h.dataOff = static_cast<uint32_t>(dataOff);
  h.dataLen = static_cast<uint32_t>(written);
  h.extOff = extLen ? static_cast<uint32_t>(extOff) : 0;
  h.extLen = extLen;
  h.stringsCrc = stringsCrc;
  h.extCrc = extCrc;
  if (esp_partition_write(part, slot, &h, sizeof(h)) != ESP_OK) {
    LOG_ERR("FLP", "header write failed");
    return false;
  }

  LOG_INF("FLP", "language %u built: %u B compressed -> %u B, %u entries, %u B extension", langIndex,
          static_cast<unsigned>(compressedLen), static_cast<unsigned>(written), keyCount,
          static_cast<unsigned>(extLen));
  return true;
}

MapResult map(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount, Mapped* out) {
  if (!out) return MapResult::Absent;

  const esp_partition_t* part = findPartition();
  if (!part) return MapResult::Absent;
  const size_t slot = slotOffset(part);
  if (slot == 0) return MapResult::Absent;

  Header h;
  if (!readHeader(part, h)) return MapResult::Absent;
  if (h.langIndex != langIndex || h.stamp != stamp || h.keyCount != keyCount) return MapResult::Absent;
  if (!layoutValid(h)) return MapResult::Absent;

  // Whole pages, and only as many as the contents span: every page mapped here is one the flash
  // font cache cannot map (the C3 shares 128 of them between the app and every mapping), so a pack
  // without an extension still costs exactly the one page it always did.
  const bool hasExt = extensionValid(h);
  bool withExt = hasExt;
  const size_t stringsEnd = h.dataOff + h.dataLen;
  for (;;) {
    const size_t need = roundUp(withExt ? static_cast<size_t>(h.extOff) + h.extLen : stringsEnd, MAP_PAGE_BYTES);
    if (s_mmapPtr && s_mapBytes >= need) break;
    unmap();
    const void* raw = nullptr;
    // The slot starts on a 64 KB boundary, which is what esp_partition_mmap requires.
    if (esp_partition_mmap(part, slot, need, ESP_PARTITION_MMAP_DATA, &raw, &s_mmapHandle) == ESP_OK) {
      s_mmapPtr = static_cast<const uint8_t*>(raw);
      s_mapBytes = need;
      break;
    }
    if (!withExt) {
      LOG_ERR("FLP", "mmap failed");
      return MapResult::MapFailed;
    }
    LOG_ERR("FLP", "mmap of %u B with the extension failed - mapping the strings alone", static_cast<unsigned>(need));
    withExt = false;
  }

  // Nothing is handed out before the bytes behind the header are proved to be the ones it was
  // written with. A corrupt table would otherwise send a string read anywhere.
  const uint8_t* base = s_mmapPtr;
  const uint32_t stringsCrc =
      crc32(crc32(0, base + h.dataOff, h.dataLen), base + h.tableOff, static_cast<size_t>(h.dataOff - h.tableOff));
  if (stringsCrc != h.stringsCrc) {
    LOG_ERR("FLP", "language %u strings fail their CRC (%08lx, header %08lx)", langIndex,
            static_cast<unsigned long>(stringsCrc), static_cast<unsigned long>(h.stringsCrc));
    return MapResult::Corrupt;
  }
  out->offsets = reinterpret_cast<const uint16_t*>(base + h.tableOff);
  out->data = reinterpret_cast<const char*>(base + h.dataOff);
  out->ext = nullptr;
  out->extLen = 0;

  MapResult result = MapResult::Ok;
  if (withExt) {
    const uint32_t extCrc = crc32(0, base + h.extOff, h.extLen);
    if (extCrc == h.extCrc) {
      out->ext = base + h.extOff;
      out->extLen = h.extLen;
    } else {
      LOG_ERR("FLP", "language %u extension fails its CRC (%08lx, header %08lx)", langIndex,
              static_cast<unsigned long>(extCrc), static_cast<unsigned long>(h.extCrc));
      result = MapResult::ExtensionCorrupt;
    }
  } else if (hasExt) {
    result = MapResult::WithoutExtension;
  }
  LOG_DBG("FLP", "language %u mapped: table=%p data=%p ext=%p (%u B)", langIndex, out->offsets, out->data, out->ext,
          static_cast<unsigned>(out->extLen));
  return result;
}

bool acquire(const uint8_t langIndex, const uint32_t stamp, const uint8_t* compressed, const size_t compressedLen,
             const size_t uncompressedLen, const uint16_t keyCount, const uint16_t* enOffsets, const Extension* ext,
             Mapped* out) {
  const uint32_t extBytes = ext ? ext->bytes : 0;
  if (holds(langIndex, stamp, extBytes)) {
    const MapResult held = map(langIndex, stamp, keyCount, out);
    if (held == MapResult::Ok || held == MapResult::WithoutExtension) return true;
    // A rebuild cannot give the strings a mapping the MMU has no page for.
    if (held == MapResult::MapFailed) return false;
    LOG_ERR("FLP", "language %u slot holds bytes it was not written with - rebuilding", langIndex);
  }
  // Either a different language, or the same one from a different firmware build — the stamp covers
  // both — or the bytes behind the header changed since. One build, whatever it leads to.
  if (!build(langIndex, stamp, compressed, compressedLen, uncompressedLen, keyCount, enOffsets, ext)) return false;
  const MapResult built = map(langIndex, stamp, keyCount, out);
  // An extension that fails even when just written is left out: the strings are what matter, and the
  // extension's reader works without it (CjkUiFonts decodes per glyph instead).
  return built == MapResult::Ok || built == MapResult::WithoutExtension || built == MapResult::ExtensionCorrupt;
}

void unmap() {
  if (!s_mmapPtr) return;
  esp_partition_munmap(s_mmapHandle);
  s_mmapHandle = 0;
  s_mmapPtr = nullptr;
  s_mapBytes = 0;
}

bool isMapped() { return s_mmapPtr != nullptr; }

}  // namespace FlashLangPartition
