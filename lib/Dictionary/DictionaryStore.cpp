#include "DictionaryStore.h"

#include <Epub/hyphenation/HyphenationCommon.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint8_t FLAG_TURKISH_KEYS = 0x01;
}

bool DictionaryStore::open(const DictByteSource& source) {
  close();
  if (source.readAt == nullptr) {
    return false;
  }

  uint8_t header[HEADER_LEN];
  if (!source.readAt(source.ctx, 0, header, HEADER_LEN)) {
    return false;
  }
  if (memcmp(header, "CPD1", 4) != 0) {
    return false;
  }

  uint32_t count = 0, idxOff = 0, entOff = 0;
  memcpy(&count, header + 4, 4);
  memcpy(&idxOff, header + 8, 4);
  memcpy(&entOff, header + 12, 4);
  const uint8_t keyLen = header[16];

  if (keyLen != KEY_LEN || count == 0 || count > MAX_ENTRY_COUNT || idxOff < HEADER_LEN) {
    return false;
  }
  if (static_cast<uint64_t>(entOff) != static_cast<uint64_t>(idxOff) + static_cast<uint64_t>(count) * RECORD_LEN) {
    return false;
  }

  src = source;
  entryCount = count;
  indexOffset = idxOff;
  entriesOffset = entOff;
  flags = header[17];
  memcpy(titleBuf, header + 32, MAX_TITLE_LEN);
  titleBuf[MAX_TITLE_LEN] = '\0';
  return true;
}

void DictionaryStore::close() {
  src = DictByteSource{};
  entryCount = 0;
  indexOffset = 0;
  entriesOffset = 0;
  flags = 0;
  titleBuf[0] = '\0';
}

// Must produce the same bytes as normalize_key() in scripts/make_dictionary.py:
// codepoint-wise Latin lowercasing (Turkish dotted/dotless I when the
// dictionary was built with --turkish-keys), UTF-8 encoded, truncated to
// KEY_LEN bytes and NUL-padded.
void DictionaryStore::normalizeKey(const char* word, char* key) const {
  memset(key, 0, KEY_LEN);
  std::string lowered;
  lowered.reserve(KEY_LEN + 4);
  const bool turkish = (flags & FLAG_TURKISH_KEYS) != 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word);
  while (*ptr != 0 && lowered.size() < static_cast<size_t>(KEY_LEN) + 4) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    utf8AppendCodepoint(turkish ? toLowerTurkish(cp) : toLowerLatin(cp), lowered);
  }
  memcpy(key, lowered.data(), std::min(lowered.size(), static_cast<size_t>(KEY_LEN)));
}

bool DictionaryStore::readRecord(uint32_t i, char* key, uint32_t& off, uint32_t& len) const {
  uint8_t rec[RECORD_LEN];
  if (!src.readAt(src.ctx, indexOffset + i * RECORD_LEN, rec, RECORD_LEN)) {
    return false;
  }
  memcpy(key, rec, KEY_LEN);
  memcpy(&off, rec + KEY_LEN, 4);
  memcpy(&len, rec + KEY_LEN + 4, 4);
  return true;
}

bool DictionaryStore::readDefinition(uint32_t off, uint32_t len, std::string& out) const {
  if (len == 0) {
    return false;
  }
  len = std::min(len, MAX_DEFINITION_BYTES);
  out.resize(len);
  if (!src.readAt(src.ctx, entriesOffset + off, &out[0], len)) {
    out.clear();
    return false;
  }
  return true;
}

bool DictionaryStore::lookup(const char* word, std::string& definition, char* matchedKey) const {
  if (!isOpen() || word == nullptr || word[0] == '\0') {
    return false;
  }

  char query[KEY_LEN];
  normalizeKey(word, query);
  if (query[0] == '\0') {
    return false;
  }

  char key[KEY_LEN];
  uint32_t off = 0, len = 0;

  // Binary search over the sorted index; `lo` ends at the insertion point.
  uint32_t lo = 0, hi = entryCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(mid, key, off, len)) {
      return false;
    }
    const int cmp = memcmp(key, query, KEY_LEN);
    if (cmp == 0) {
      memcpy(matchedKey, key, KEY_LEN);
      matchedKey[KEY_LEN] = '\0';
      return readDefinition(off, len, definition);
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  // Exact miss: walk backwards from the insertion point looking for the
  // longest indexed key that is a proper prefix of the query. Keys are
  // sorted, so the first prefix found is the longest one.
  const size_t queryLen = strnlen(query, KEY_LEN);
  uint32_t scanned = 0;
  for (uint32_t i = lo; i > 0 && scanned < PREFIX_SCAN_LIMIT; --i, ++scanned) {
    if (!readRecord(i - 1, key, off, len)) {
      return false;
    }
    if (key[0] != query[0]) {
      break;  // sorted index: no earlier key can share the first byte
    }
    const size_t keyLenBytes = strnlen(key, KEY_LEN);
    if (keyLenBytes < MIN_PREFIX_KEY_LEN || keyLenBytes >= queryLen) {
      continue;
    }
    if (memcmp(key, query, keyLenBytes) == 0) {
      memcpy(matchedKey, key, KEY_LEN);
      matchedKey[KEY_LEN] = '\0';
      return readDefinition(off, len, definition);
    }
  }

  return false;
}
