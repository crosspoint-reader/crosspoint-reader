#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>

// On-disk index entry: fixed 36 bytes for O(1) random access during binary search.
// NOTE: Do NOT use __attribute__((packed)) — ESP32-C3 RISC-V faults on unaligned access.
// These structs are naturally aligned (char[] + uint32_t).
// Read/write via memcpy to be safe when reading from file buffers.
static constexpr int DICT_INDEX_ENTRY_SIZE = 36;   // char[32] + uint32_t
static constexpr int DICT_INDEX_HEADER_SIZE = 12;  // 3 x uint32_t
static constexpr int DICT_WORD_MAX = 32;           // Max word length in index (including null)

struct DictIndexEntry {
  char word[DICT_WORD_MAX];  // Null-terminated, truncated if > 31 chars
  uint32_t byteOffset;       // Offset into source .dict file
};
static_assert(sizeof(DictIndexEntry) == DICT_INDEX_ENTRY_SIZE,
              "DictIndexEntry size mismatch — update DICT_INDEX_ENTRY_SIZE");

struct DictIndexHeader {
  uint32_t dictFileSize;   // Size of source .dict file
  uint32_t spotCheckHash;  // FNV-1a of first + middle + last lines
  uint32_t entryCount;     // Number of entries in index
};
static_assert(sizeof(DictIndexHeader) == DICT_INDEX_HEADER_SIZE,
              "DictIndexHeader size mismatch — update DICT_INDEX_HEADER_SIZE");

// Result from a single dictionary lookup
struct DictResult {
  char dictionaryName[32];  // Title-cased display name
  char definition[2048];    // Definition text, null-terminated, truncated if too long
};

// Info about a discovered dictionary file
struct DictFileInfo {
  char filename[64];     // e.g., "english" (no extension)
  char displayName[64];  // e.g., "English" (title-cased)
  bool enabled = false;
  bool corrupt = false;   // Failed validation
  bool readOnly = false;  // Index generation failed
};

// Title-case a dictionary filename into a display name.
// Replaces underscores with spaces. Capitalizes after word boundaries
// (start, space, hyphen, opening paren). Lowercases everything else.
// Output written to `out`, max `outSize` bytes including null terminator.
inline void titleCaseDictName(const char* filename, char* out, int outSize) {
  if (outSize <= 0) return;
  bool capitalizeNext = true;
  int j = 0;
  for (int i = 0; filename[i] != '\0' && j < outSize - 1; ++i) {
    char c = filename[i];
    if (c == '_') {
      out[j++] = ' ';
      capitalizeNext = true;
    } else if (c == '-' || c == '(') {
      out[j++] = c;
      capitalizeNext = true;
    } else if (capitalizeNext) {
      out[j++] = static_cast<char>(toupper(static_cast<unsigned char>(c)));
      capitalizeNext = false;
    } else {
      out[j++] = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      capitalizeNext = false;
    }
  }
  out[j] = '\0';
}

// FNV-1a hash for spot-check validation
inline uint32_t fnv1aHash(const char* data, int len) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < len; ++i) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}
