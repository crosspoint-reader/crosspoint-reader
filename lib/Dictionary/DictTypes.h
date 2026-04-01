#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>

// Max word length stored in secondary index (including null terminator).
static constexpr int DICT_WORD_MAX = 32;

// ---------------------------------------------------------------------------
// Secondary index (.idx.cp) — generated from StarDict .idx for O(1) binary search.
// NOTE: Do NOT use __attribute__((packed)) — ESP32-C3 RISC-V faults on unaligned access.
// These structs are naturally aligned (char[] + uint32_t).
// Read/write via memcpy to be safe when reading from file buffers.
// ---------------------------------------------------------------------------
static constexpr uint32_t SD_INDEX_MAGIC = 0x43504958;  // "CPIX"
static constexpr uint32_t SD_INDEX_VERSION = 1;
static constexpr int SD_INDEX_HEADER_SIZE = 16;
static constexpr int SD_INDEX_ENTRY_SIZE = 44;

struct SdIndexHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t idxFileSize;  // StarDict .idx file size (for invalidation)
  uint32_t entryCount;
};
static_assert(sizeof(SdIndexHeader) == SD_INDEX_HEADER_SIZE,
              "SdIndexHeader size mismatch — update SD_INDEX_HEADER_SIZE");

struct SdIndexEntry {
  char word[DICT_WORD_MAX];  // 32 bytes, truncated for binary search comparison
  uint32_t dictOffset;        // Byte offset into .dict (little-endian, converted from BE)
  uint32_t dictSize;          // Byte size of definition in .dict
  uint32_t idxWordOffset;     // Byte offset of full word in StarDict .idx (for >31 char verification)
};
static_assert(sizeof(SdIndexEntry) == SD_INDEX_ENTRY_SIZE,
              "SdIndexEntry size mismatch — update SD_INDEX_ENTRY_SIZE");

// ---------------------------------------------------------------------------
// Result from a single dictionary lookup
// ---------------------------------------------------------------------------
struct DictResult {
  char dictionaryName[32];  // Display name from .ifo bookname
  char definition[2048];    // Definition text, null-terminated, truncated if too long
};

// Info about a discovered dictionary (one per .ifo file found)
struct DictFileInfo {
  char filename[64];     // Path relative to DICT_DIR without extension, e.g., "english" or "wordnet-en/wordnet"
  char displayName[64];  // From .ifo bookname, or title-cased filename as fallback
  bool enabled = false;
  bool corrupt = false;   // Failed validation (missing .idx/.dict, bad format)
  bool readOnly = false;  // Secondary index generation failed (SD write error)
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
