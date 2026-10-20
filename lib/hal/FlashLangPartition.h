#pragma once

#include <cstddef>
#include <cstdint>

// The UI language pack: the active language's strings, decompressed once into
// the tail of the spiffs partition and read back through mmap.
//
// Why this exists. Shipping all 24 languages as rodata cost 408,668 B of the app
// image, of which any one reader uses exactly one. The image now carries English
// uncompressed plus a deflate-compressed blob per language (~6 KB each); this
// materialises whichever one is selected so I18n::get() can still hand out a
// plain `const char*` with no RAM standing behind it. An mmap'd flash pointer
// reads exactly like rodata — that equivalence is the whole point, and it is why
// the alternative (decompressing into a heap buffer) was rejected: ~30 KB
// resident on a device whose steady-state free heap at Home is under 40 KB.
//
// Slot layout, at (partition size - FlashFontPartition::LANG_RESERVED_BYTES):
//
//   [ 4 B] magic "CPLP"
//   [ 1 B] format version
//   [ 1 B] language index
//   [ 2 B] key count
//   [ 4 B] stamp — identifies which firmware build's pack this is
//   [ 4 B] table offset, from slot start
//   [ 4 B] data offset
//   [ 4 B] data length
//   [ 4 B] extension offset, from slot start (0: none)
//   [ 4 B] extension length (0: none)
//   [ 4 B] CRC32 of the strings: the data, then the table, as build() wrote them
//   [ 4 B] CRC32 of the extension, as build() wrote it (0: none)
//   [table] keyCount x uint16_t — offsets into data; bit 15 set means "fall back
//           to English", with the low 15 bits being the offset into the English
//           blob, matching what the generator emits for in-image languages
//   [data]  keyCount NUL-terminated strings, in StrId order. An EMPTY string
//           means the language does not override English for that key; the
//           table entry carries the English offset instead, so the empty byte
//           is never read.
//   [ext]   optional, 4-byte aligned: bytes an Extension writes after the
//           strings, built, erased and mapped with them. The CJK languages keep
//           their CJK UI faces here (src/CjkUiFonts.cpp), inflated from the
//           compressed faces in the image so they read like rodata.
//
// The header is written LAST. A power cut part-way through build() therefore
// leaves no magic, and the slot reads as absent rather than as plausible
// garbage — the reader falls back to English and rebuilds on the next boot.
//
// Why the contents carry CRCs (format 2). The header alone cannot prove the
// bytes behind it are the ones it describes, because other firmware writes this
// flash too. A firmware from before the 128 KB slot keeps its own 64 KB slot in
// this slot's upper half, and its font cache and TTF glyph store in the lower
// half; any of them can rewrite sectors here and leave this header standing.
// Measured on the X3: back from such a firmware, the Chinese pack still
// "matched", and its 12 pt CJK face drew as solid boxes from the bytes that
// firmware had written over its tail. So map() checks both CRCs before it hands
// out a pointer, and acquire() rebuilds a slot that fails them.

namespace FlashLangPartition {

static constexpr size_t HEADER_BYTES = 40;
// 2: the CRCs. A format-1 slot reads as absent and is rebuilt once.
static constexpr uint8_t FORMAT_VERSION = 2;

// The largest back-reference distance the generator's deflate encoder may use.
// It compresses with a 4 KB window precisely so the ring below can be 4 KB
// instead of uzlib's default 32 KB; the two numbers must agree or long matches
// decode to garbage. See gen_i18n.py's DEFLATE_WINDOW_BITS.
static constexpr size_t INFLATE_RING_BYTES = 4096;

struct Mapped {
  const uint16_t* offsets;
  const char* data;
  // The extension, mapped and verified; nullptr when the slot carries none, or
  // it could not be mapped with the strings, or it failed its CRC.
  const uint8_t* ext;
  uint32_t extLen;
};

// Where an Extension writes. Buffered: the bytes reach flash in WRITE_CHUNK
// pieces, because each esp_partition_write() costs ~100 us on top of its bytes.
class SlotWriter {
 public:
  // Appends `len` bytes. False on a flash error or a write past the declared
  // length; the producer stops then.
  virtual bool write(const void* data, size_t len) = 0;
  // A buffer the producer may use while it runs: the inflate ring the strings
  // were unpacked through, free by the time the extension is written. Saves
  // the producer an allocation of its own.
  virtual uint8_t* scratch() = 0;
  virtual size_t scratchBytes() const = 0;

 protected:
  ~SlotWriter() = default;
};

// Bytes the slot carries after the strings. `produce` must write exactly
// `bytes` of them; it returning false, or writing fewer, abandons the extension
// and leaves the strings in place.
struct Extension {
  uint32_t bytes;
  bool (*produce)(void* ctx, SlotWriter& out);
  void* ctx;
};

// True when the slot's HEADER describes exactly this (language, stamp) pair with
// an extension of `extBytes` (0: none). Cheap, and not proof: whether the bytes
// behind the header are still the ones it describes is what map() checks. An
// extension that failed to build last time does not match, so it is retried.
bool holds(uint8_t langIndex, uint32_t stamp, uint32_t extBytes);

// Decompress `compressed` into the slot and derive the offset table from it,
// then write `ext` after it when given. `enOffsets` supplies the English offset
// for every entry the blob leaves empty. Returns false and leaves the slot
// without a valid header on any failure of the strings, so a failed build
// degrades to English rather than to corruption. A failed extension does not
// fail the build: the header records none, and map() hands out the strings
// alone.
//
// Transient cost while running: a 4 KB inflate ring, a 512 B write staging
// buffer and keyCount x 2 B for the table being derived (~6.3 KB at 887 keys);
// the extension reuses the first two. Nothing is retained afterwards.
bool build(uint8_t langIndex, uint32_t stamp, const uint8_t* compressed, size_t compressedLen, size_t uncompressedLen,
           uint16_t keyCount, const uint16_t* enOffsets, const Extension* ext = nullptr);

// What map() found.
enum class MapResult : uint8_t {
  Ok,                // the strings, and the extension if the slot has one, mapped and verified
  WithoutExtension,  // the strings mapped and verified; the extension could not be mapped with them
  ExtensionCorrupt,  // the strings mapped and verified; the extension failed its CRC and is withheld
  Corrupt,           // the strings failed their CRC: nothing handed out
  Absent,            // no slot for this language, stamp and key count, or a malformed header
  MapFailed,         // the strings could not be mapped at all
};

// Map the slot for reading — as many 64 KB pages as its contents span, the
// extension included — and verify what it maps against the header's CRCs
// before handing any of it out: one read of the mapped bytes, ~88 KB for
// Chinese. `out` is filled only for Ok, WithoutExtension and ExtensionCorrupt.
MapResult map(uint8_t langIndex, uint32_t stamp, uint16_t keyCount, Mapped* out);

// Everything I18n wants from the slot, in one call: the pack of `langIndex`
// mapped and verified, built first when the slot does not hold it and REBUILT
// when it holds it but fails its CRCs, then mapped once more. One build at
// most, whatever happens, so a slot that keeps failing costs one rebuild per
// call rather than a loop. After a rebuild an extension that still fails is
// left out and the strings stand alone. False: no strings could be had (the
// caller stays on English).
bool acquire(uint8_t langIndex, uint32_t stamp, const uint8_t* compressed, size_t compressedLen, size_t uncompressedLen,
             uint16_t keyCount, const uint16_t* enOffsets, const Extension* ext, Mapped* out);

// Release the mapping. No-op if not mapped.
void unmap();

bool isMapped();

}  // namespace FlashLangPartition
