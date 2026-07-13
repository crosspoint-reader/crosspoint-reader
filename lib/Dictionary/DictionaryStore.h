#pragma once

#include <cstdint>
#include <string>

// Random-access byte source so DictionaryStore can be unit-tested on host and
// backed by a HalFile on device.
struct DictByteSource {
  void* ctx = nullptr;
  // Read exactly `len` bytes at absolute file offset `off`. Returns false on
  // a short read or I/O error.
  bool (*readAt)(void* ctx, uint32_t off, void* buf, uint32_t len) = nullptr;
};

// Reader for the .cpd dictionary format produced by scripts/make_dictionary.py.
// The sorted fixed-width index is binary-searched through the byte source
// (one 36-byte read per probe), so lookups use O(1) RAM regardless of
// dictionary size.
class DictionaryStore {
 public:
  static constexpr uint8_t KEY_LEN = 28;
  static constexpr uint32_t HEADER_LEN = 96;
  static constexpr uint32_t RECORD_LEN = KEY_LEN + 8;
  static constexpr uint32_t MAX_DEFINITION_BYTES = 8192;
  static constexpr size_t MAX_TITLE_LEN = 64;

  // Parses and validates the header. Returns false on bad magic or geometry.
  // Keeps a copy of `source`; the underlying file must stay open while this
  // store is used.
  bool open(const DictByteSource& source);
  void close();
  bool isOpen() const { return entryCount != 0; }
  const char* title() const { return titleBuf; }

  // Looks up `word` (any case; the caller strips surrounding punctuation).
  // On an exact miss, falls back to the longest indexed key that is a proper
  // prefix of the normalized word, which handles agglutinated forms like
  // "kitaplarımızdan" -> "kitap". On success fills `definition` and copies
  // the NUL-terminated matched headword into `matchedKey` (KEY_LEN + 1 bytes).
  bool lookup(const char* word, std::string& definition, char* matchedKey) const;

 private:
  static constexpr uint32_t MAX_ENTRY_COUNT = 4000000;
  static constexpr uint32_t PREFIX_SCAN_LIMIT = 64;
  static constexpr size_t MIN_PREFIX_KEY_LEN = 3;

  bool readRecord(uint32_t i, char* key, uint32_t& off, uint32_t& len) const;
  bool readDefinition(uint32_t off, uint32_t len, std::string& out) const;
  void normalizeKey(const char* word, char* key) const;

  DictByteSource src{};
  uint32_t entryCount = 0;
  uint32_t indexOffset = 0;
  uint32_t entriesOffset = 0;
  uint8_t flags = 0;
  char titleBuf[MAX_TITLE_LEN + 1] = {};
};
