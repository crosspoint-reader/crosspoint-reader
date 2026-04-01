#include "StarDictIndex.h"

#include <Logging.h>

#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read a big-endian uint32 from a byte buffer (RISC-V alignment safe).
static uint32_t readBE32(const uint8_t* buf) {
  return (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
         (static_cast<uint32_t>(buf[2]) << 8) | static_cast<uint32_t>(buf[3]);
}

// Buffered wrapper around HalFile for efficient sequential reads.
// Reads IO_BUF_SIZE chunks from SD instead of single bytes, giving ~100x speedup.
struct BufferedReader {
  static constexpr int IO_BUF_SIZE = 512;
  HalFile& file;
  char ioBuf[IO_BUF_SIZE];
  int bufPos = 0;
  int bufLen = 0;

  explicit BufferedReader(HalFile& f) : file(f) {}

  void invalidate() {
    bufPos = 0;
    bufLen = 0;
  }

  int readByte() {
    if (bufPos >= bufLen) {
      bufLen = file.read(ioBuf, IO_BUF_SIZE);
      bufPos = 0;
      if (bufLen <= 0) return -1;
    }
    return static_cast<unsigned char>(ioBuf[bufPos++]);
  }

  // Read exactly n bytes into buf. Returns false if fewer than n bytes available.
  bool readBytes(void* buf, int n) {
    auto* dst = static_cast<uint8_t*>(buf);
    for (int i = 0; i < n; ++i) {
      int b = readByte();
      if (b < 0) return false;
      dst[i] = static_cast<uint8_t>(b);
    }
    return true;
  }

  uint32_t position() const { return static_cast<uint32_t>(file.position()) - (bufLen - bufPos); }

  void seekSet(uint32_t pos) {
    file.seekSet(pos);
    invalidate();
  }

  bool eof() const { return bufPos >= bufLen && file.available() <= 0; }
};

// Build a full path by combining basePath + extension into outPath.
static bool buildPath(const char* basePath, const char* ext, char* outPath, int outSize) {
  int baseLen = static_cast<int>(strlen(basePath));
  int extLen = static_cast<int>(strlen(ext));
  if (baseLen + extLen + 1 > outSize) return false;
  memcpy(outPath, basePath, baseLen);
  memcpy(outPath + baseLen, ext, extLen + 1);  // includes null
  return true;
}

// ---------------------------------------------------------------------------
// parseIfo — read StarDict .ifo metadata
// ---------------------------------------------------------------------------
bool StarDictIndex::parseIfo(const char* ifoPath, char* outBookname, int nameSize, uint32_t* outWordCount,
                             uint32_t* outIdxFileSize, char* outSameTypeSeq, int seqSize) {
  if (nameSize > 0) outBookname[0] = '\0';
  if (seqSize > 0) outSameTypeSeq[0] = '\0';
  if (outWordCount) *outWordCount = 0;
  if (outIdxFileSize) *outIdxFileSize = 0;

  HalFile file;
  if (!Storage.openFileForRead("DICT", ifoPath, file)) return false;

  // Read entire .ifo (typically < 500 bytes)
  static constexpr int IFO_BUF_SIZE = 1024;
  char buf[IFO_BUF_SIZE];
  int bytesRead = file.read(buf, IFO_BUF_SIZE - 1);
  file.close();
  if (bytesRead <= 0) return false;
  buf[bytesRead] = '\0';

  // Verify magic first line
  const char* magic = "StarDict's dict ifo file";
  if (strncmp(buf, magic, strlen(magic)) != 0) {
    LOG_ERR("DICT", "Invalid .ifo magic: %s", ifoPath);
    return false;
  }

  // Parse key=value lines
  const char* p = buf;
  while (*p) {
    // Skip to start of next line
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    if (!*p) break;

    // Find '='
    const char* eq = strchr(p, '=');
    const char* lineEnd = strchr(p, '\n');
    if (!lineEnd) lineEnd = buf + bytesRead;
    if (!eq || eq > lineEnd) continue;

    int keyLen = static_cast<int>(eq - p);
    const char* val = eq + 1;
    int valLen = static_cast<int>(lineEnd - val);
    // Strip trailing \r
    if (valLen > 0 && val[valLen - 1] == '\r') valLen--;

    if (keyLen == 8 && memcmp(p, "bookname", 8) == 0) {
      int copyLen = (valLen < nameSize - 1) ? valLen : nameSize - 1;
      memcpy(outBookname, val, copyLen);
      outBookname[copyLen] = '\0';
    } else if (keyLen == 9 && memcmp(p, "wordcount", 9) == 0) {
      if (outWordCount) {
        char tmp[16];
        int copyLen = (valLen < 15) ? valLen : 15;
        memcpy(tmp, val, copyLen);
        tmp[copyLen] = '\0';
        *outWordCount = static_cast<uint32_t>(strtoul(tmp, nullptr, 10));
      }
    } else if (keyLen == 11 && memcmp(p, "idxfilesize", 11) == 0) {
      if (outIdxFileSize) {
        char tmp[16];
        int copyLen = (valLen < 15) ? valLen : 15;
        memcpy(tmp, val, copyLen);
        tmp[copyLen] = '\0';
        *outIdxFileSize = static_cast<uint32_t>(strtoul(tmp, nullptr, 10));
      }
    } else if (keyLen == 16 && memcmp(p, "sametypesequence", 16) == 0) {
      if (seqSize > 0) {
        int copyLen = (valLen < seqSize - 1) ? valLen : seqSize - 1;
        memcpy(outSameTypeSeq, val, copyLen);
        outSameTypeSeq[copyLen] = '\0';
      }
    }
  }

  return outBookname[0] != '\0';  // bookname is required
}

// ---------------------------------------------------------------------------
// ensureIndex
// ---------------------------------------------------------------------------
bool StarDictIndex::ensureIndex(const char* basePath, bool& outCorrupt, bool& outReadOnly) {
  outCorrupt = false;
  outReadOnly = false;

  char idxPath[128], cpIdxPath[128];
  if (!buildPath(basePath, ".idx", idxPath, sizeof(idxPath)) ||
      !buildPath(basePath, ".idx.cp", cpIdxPath, sizeof(cpIdxPath))) {
    LOG_ERR("DICT", "Path too long for: %s", basePath);
    return false;
  }

  if (Storage.exists(cpIdxPath) && validateIndex(idxPath, cpIdxPath)) {
    LOG_INF("DICT", "Secondary index valid: %s", cpIdxPath);
    return true;
  }

  LOG_INF("DICT", "Generating secondary index for: %s", basePath);
  if (!generateIndex(idxPath, cpIdxPath, outCorrupt)) {
    if (outCorrupt) {
      LOG_ERR("DICT", "StarDict .idx is corrupt: %s", idxPath);
    } else {
      LOG_ERR("DICT", "Could not write secondary index (read-only?): %s", cpIdxPath);
      outReadOnly = true;
    }
    return false;
  }

  LOG_INF("DICT", "Secondary index generated: %s", cpIdxPath);
  return true;
}

// ---------------------------------------------------------------------------
// validateIndex
// ---------------------------------------------------------------------------
bool StarDictIndex::validateIndex(const char* idxPath, const char* cpIdxPath) {
  // Get StarDict .idx file size for invalidation check
  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", idxPath, idxFile)) return false;
  uint32_t idxSize = static_cast<uint32_t>(idxFile.size());
  idxFile.close();

  HalFile cpFile;
  if (!Storage.openFileForRead("DICT", cpIdxPath, cpFile)) return false;
  uint32_t cpSize = static_cast<uint32_t>(cpFile.size());

  SdIndexHeader header;
  if (cpFile.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    cpFile.close();
    return false;
  }
  cpFile.close();

  if (header.magic != SD_INDEX_MAGIC || header.version != SD_INDEX_VERSION) {
    LOG_INF("DICT", "Secondary index magic/version mismatch");
    return false;
  }
  if (header.idxFileSize != idxSize) {
    LOG_INF("DICT", "Secondary index .idx size mismatch: expected %u, got %u", idxSize, header.idxFileSize);
    return false;
  }
  uint64_t expectedSize = SD_INDEX_HEADER_SIZE + static_cast<uint64_t>(header.entryCount) * sizeof(SdIndexEntry);
  if (header.entryCount == 0 || cpSize != expectedSize) {
    LOG_INF("DICT", "Secondary index file size mismatch: expected %u, got %u", expectedSize, cpSize);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// generateIndex — scan StarDict .idx, write .idx.cp secondary index
// ---------------------------------------------------------------------------
bool StarDictIndex::generateIndex(const char* idxPath, const char* cpIdxPath, bool& outCorrupt) {
  outCorrupt = false;

  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", idxPath, idxFile)) return false;
  uint32_t idxSize = static_cast<uint32_t>(idxFile.size());

  BufferedReader reader(idxFile);

  // First pass: count entries to write header
  uint32_t entryCount = 0;
  reader.seekSet(0);
  while (!reader.eof()) {
    uint32_t wordStart = reader.position();
    (void)wordStart;

    // Read null-terminated word
    int wordLen = 0;
    while (true) {
      int b = reader.readByte();
      if (b < 0) goto countDone;
      if (b == 0) break;
      wordLen++;
      if (wordLen > 255) {
        LOG_ERR("DICT", "Word exceeds 255 bytes at entry %u", entryCount);
        outCorrupt = true;
        idxFile.close();
        return false;
      }
    }
    if (wordLen == 0) {
      outCorrupt = true;
      idxFile.close();
      return false;
    }

    // Skip 8 bytes (offset + size, both uint32 big-endian)
    uint8_t skip[8];
    if (!reader.readBytes(skip, 8)) {
      LOG_ERR("DICT", "Unexpected EOF reading offset/size at entry %u", entryCount);
      outCorrupt = true;
      idxFile.close();
      return false;
    }
    entryCount++;
  }
countDone:

  if (entryCount == 0) {
    LOG_ERR("DICT", "StarDict .idx is empty: %s", idxPath);
    outCorrupt = true;
    idxFile.close();
    return false;
  }

  // Second pass: write secondary index
  HalFile cpFile;
  if (!Storage.openFileForWrite("DICT", cpIdxPath, cpFile)) {
    idxFile.close();
    return false;
  }

  SdIndexHeader header;
  header.magic = SD_INDEX_MAGIC;
  header.version = SD_INDEX_VERSION;
  header.idxFileSize = idxSize;
  header.entryCount = entryCount;
  if (cpFile.write(&header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("DICT", "Failed to write secondary index header");
    cpFile.close();
    idxFile.close();
    return false;
  }

  reader.seekSet(0);
  char wordBuf[256];
  for (uint32_t i = 0; i < entryCount; ++i) {
    uint32_t wordStart = reader.position();

    // Read null-terminated word
    int wordLen = 0;
    while (true) {
      int b = reader.readByte();
      if (b < 0 || b == 0) break;
      if (wordLen < 255) wordBuf[wordLen] = static_cast<char>(b);
      wordLen++;
    }
    int truncLen = (wordLen < DICT_WORD_MAX - 1) ? wordLen : DICT_WORD_MAX - 1;

    // Read offset and size (big-endian)
    uint8_t raw[8];
    if (!reader.readBytes(raw, 8)) break;

    SdIndexEntry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.word, wordBuf, truncLen);
    entry.word[truncLen] = '\0';
    entry.dictOffset = readBE32(raw);
    entry.dictSize = readBE32(raw + 4);
    entry.idxWordOffset = wordStart;

    if (cpFile.write(&entry, sizeof(entry)) != sizeof(entry)) {
      LOG_ERR("DICT", "Failed to write secondary index entry %u", i);
      cpFile.close();
      idxFile.close();
      return false;
    }
  }

  cpFile.close();
  idxFile.close();
  return true;
}

// ---------------------------------------------------------------------------
// binarySearchIndex — search .idx.cp for a word
// ---------------------------------------------------------------------------
int32_t StarDictIndex::binarySearchIndex(HalFile& cpIdxFile, uint32_t entryCount, const char* word) {
  int32_t lo = 0;
  int32_t hi = static_cast<int32_t>(entryCount) - 1;
  int32_t bestMatch = -1;

  while (lo <= hi) {
    int32_t mid = lo + (hi - lo) / 2;
    size_t offset = SD_INDEX_HEADER_SIZE + static_cast<size_t>(mid) * sizeof(SdIndexEntry);
    cpIdxFile.seekSet(offset);

    SdIndexEntry entry;
    if (cpIdxFile.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) {
      LOG_ERR("DICT", "Failed to read secondary index entry at %d", mid);
      return -1;
    }
    entry.word[DICT_WORD_MAX - 1] = '\0';

    int cmp = strncasecmp(word, entry.word, DICT_WORD_MAX - 1);
    if (cmp == 0) {
      bestMatch = mid;
      hi = mid - 1;  // find leftmost match
    } else if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }

  return bestMatch;
}

// ---------------------------------------------------------------------------
// lookup — binary search in .idx.cp, read definition from .dict
// ---------------------------------------------------------------------------
bool StarDictIndex::lookup(const char* basePath, const char* word, char* outDef, int outDefSize) {
  if (outDefSize < 4) {
    if (outDefSize > 0) outDef[0] = '\0';
    return false;
  }
  outDef[0] = '\0';

  char cpIdxPath[128], dictPath[128], idxPath[128];
  if (!buildPath(basePath, ".idx.cp", cpIdxPath, sizeof(cpIdxPath)) ||
      !buildPath(basePath, ".dict", dictPath, sizeof(dictPath)) ||
      !buildPath(basePath, ".idx", idxPath, sizeof(idxPath))) {
    return false;
  }

  HalFile cpIdxFile;
  if (!Storage.openFileForRead("DICT", cpIdxPath, cpIdxFile)) return false;

  SdIndexHeader header;
  if (cpIdxFile.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    cpIdxFile.close();
    return false;
  }

  int32_t idx = binarySearchIndex(cpIdxFile, header.entryCount, word);
  if (idx < 0) {
    cpIdxFile.close();
    return false;
  }

  // Open .dict for definition reads, and .idx for full-word verification of long words
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) {
    cpIdxFile.close();
    return false;
  }

  // Walk adjacent entries sharing the same truncated prefix (handles words > 31 chars).
  // The strncasecmp check below stops iteration once the prefix no longer matches.
  bool found = false;
  for (int32_t candidate = idx; candidate < static_cast<int32_t>(header.entryCount); ++candidate) {
    size_t entryOffset = SD_INDEX_HEADER_SIZE + static_cast<size_t>(candidate) * sizeof(SdIndexEntry);
    cpIdxFile.seekSet(entryOffset);
    SdIndexEntry entry;
    if (cpIdxFile.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) break;
    entry.word[DICT_WORD_MAX - 1] = '\0';

    // Stop once truncated prefix no longer matches
    if (strncasecmp(word, entry.word, DICT_WORD_MAX - 1) != 0) break;

    // For short words, the truncated word is the full word — compare directly.
    // For words that may have been truncated (fills all 31 chars), verify from .idx.
    int truncLen = static_cast<int>(strlen(entry.word));
    if (truncLen >= DICT_WORD_MAX - 1) {
      // Read full word from StarDict .idx for exact comparison
      HalFile sdIdxFile;
      if (!Storage.openFileForRead("DICT", idxPath, sdIdxFile)) continue;
      sdIdxFile.seekSet(entry.idxWordOffset);
      char fullWord[256];
      int fwLen = 0;
      while (fwLen < 255) {
        uint8_t b;
        if (sdIdxFile.read(&b, 1) != 1 || b == 0) break;
        fullWord[fwLen++] = static_cast<char>(b);
      }
      fullWord[fwLen] = '\0';
      sdIdxFile.close();

      if (strcasecmp(word, fullWord) != 0) continue;
    } else {
      if (strcasecmp(word, entry.word) != 0) continue;
    }

    // Match found — read definition from .dict by offset+size
    int readSize = static_cast<int>(entry.dictSize);
    if (readSize >= outDefSize) readSize = outDefSize - 4;  // leave room for "..."

    dictFile.seekSet(entry.dictOffset);
    int bytesRead = dictFile.read(outDef, readSize);
    if (bytesRead <= 0) continue;

    if (static_cast<int>(entry.dictSize) >= outDefSize) {
      memcpy(outDef + readSize, "...", 4);  // includes null
    } else {
      outDef[bytesRead] = '\0';
    }

    found = true;
    break;
  }

  dictFile.close();
  cpIdxFile.close();
  return found;
}
