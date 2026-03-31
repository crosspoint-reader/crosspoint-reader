#include "DictionaryIndex.h"

#include <Logging.h>

#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Buffered wrapper around HalFile for efficient sequential reads.
// Reads IO_BUF_SIZE chunks from SD instead of single bytes, giving ~100x speedup.
// Must call invalidate() after any seekSet/seekCur on the underlying file.
struct BufferedReader {
  static constexpr int IO_BUF_SIZE = 512;
  HalFile& file;
  char ioBuf[IO_BUF_SIZE];
  int bufPos = 0;
  int bufLen = 0;

  explicit BufferedReader(HalFile& f) : file(f) {}

  // Call after any external seek on the file to reset the read buffer.
  void invalidate() {
    bufPos = 0;
    bufLen = 0;
  }

  // Returns next byte or -1 at EOF.
  int readByte() {
    if (bufPos >= bufLen) {
      bufLen = file.read(ioBuf, IO_BUF_SIZE);
      bufPos = 0;
      if (bufLen <= 0) return -1;
    }
    return static_cast<unsigned char>(ioBuf[bufPos++]);
  }

  // Returns the file position of the next byte to be read.
  uint32_t position() const { return static_cast<uint32_t>(file.position()) - (bufLen - bufPos); }

  // Seek and invalidate buffer.
  void seekSet(uint32_t pos) {
    file.seekSet(pos);
    invalidate();
  }

  bool eof() const { return bufPos >= bufLen && file.available() <= 0; }
};

// Read one line from file, stripping \r\n. Returns number of chars placed in
// buf (0 at EOF). buf is always null-terminated.
static int readLine(BufferedReader& reader, char* buf, int bufSize) {
  int i = 0;
  while (i < bufSize - 1) {
    int c = reader.readByte();
    if (c < 0) break;      // EOF
    if (c == '\n') break;  // end of line
    if (c == '\r') {
      // consume \n after \r if present
      int next = reader.readByte();
      if (next >= 0 && next != '\n') {
        // Not \r\n — can't easily push back into buffer, but \r-only
        // line endings are rare. Seek to correct position.
        reader.seekSet(reader.position() - 1);
      }
      break;
    }
    buf[i++] = static_cast<char>(c);
  }
  // If buffer filled before reaching end-of-line, drain the rest of the line
  // so the next readLine call starts at the beginning of the next line.
  if (i == bufSize - 1) {
    while (true) {
      int c = reader.readByte();
      if (c < 0 || c == '\n') break;
      if (c == '\r') {
        int next = reader.readByte();
        if (next >= 0 && next != '\n') {
          reader.seekSet(reader.position() - 1);
        }
        break;
      }
    }
  }
  buf[i] = '\0';
  return i;
}

// Overload for direct HalFile (used by lookup/linearScan where only one line is read)
static int readLine(HalFile& file, char* buf, int bufSize) {
  BufferedReader reader(file);
  return readLine(reader, buf, bufSize);
}

// Build the index path by appending ".idx" to the dict path.
// Returns false if the result would overflow the buffer.
static bool buildIdxPath(const char* dictPath, char* idxPath, int idxPathSize) {
  int len = static_cast<int>(strlen(dictPath));
  if (len + 5 > idxPathSize) return false;  // ".idx" + null
  memcpy(idxPath, dictPath, len);
  memcpy(idxPath + len, ".idx", 5);  // includes null
  return true;
}

// ---------------------------------------------------------------------------
// ensureIndex
// ---------------------------------------------------------------------------
bool DictionaryIndex::ensureIndex(const char* dictPath, bool& outCorrupt, bool& outReadOnly) {
  outCorrupt = false;
  outReadOnly = false;

  char idxPath[128];
  if (!buildIdxPath(dictPath, idxPath, sizeof(idxPath))) {
    LOG_ERR("DICT", "Index path too long for: %s", dictPath);
    return false;
  }

  // If index already exists and is valid, we are done.
  if (Storage.exists(idxPath) && validateIndex(dictPath, idxPath)) {
    LOG_INF("DICT", "Index valid: %s", idxPath);
    return true;
  }

  // Need to (re)generate index.
  LOG_INF("DICT", "Generating index for: %s", dictPath);
  if (!generateIndex(dictPath, idxPath, outCorrupt)) {
    if (outCorrupt) {
      LOG_ERR("DICT", "Dict file is corrupt: %s", dictPath);
    } else {
      LOG_ERR("DICT", "Could not write index (read-only?): %s", idxPath);
      outReadOnly = true;
    }
    return false;
  }

  LOG_INF("DICT", "Index generated: %s", idxPath);
  return true;
}

// ---------------------------------------------------------------------------
// validateIndex
// ---------------------------------------------------------------------------
bool DictionaryIndex::validateIndex(const char* dictPath, const char* idxPath) {
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) return false;
  uint32_t dictSize = static_cast<uint32_t>(dictFile.size());
  uint32_t hash = computeSpotCheckHash(dictFile, dictSize);
  dictFile.close();

  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", idxPath, idxFile)) return false;

  uint32_t idxSize = static_cast<uint32_t>(idxFile.size());

  DictIndexHeader header;
  if (idxFile.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    idxFile.close();
    return false;
  }
  idxFile.close();

  // Verify index file size matches expected: header + entryCount * entry size
  uint32_t expectedSize = DICT_INDEX_HEADER_SIZE + header.entryCount * static_cast<uint32_t>(sizeof(DictIndexEntry));
  if (header.entryCount == 0 || idxSize != expectedSize) {
    LOG_INF("DICT", "Index file size mismatch: expected %u, got %u", expectedSize, idxSize);
    return false;
  }

  if (header.dictFileSize != dictSize) {
    LOG_INF("DICT", "Index size mismatch: expected %u, got %u", dictSize, header.dictFileSize);
    return false;
  }
  if (header.spotCheckHash != hash) {
    LOG_INF("DICT", "Index hash mismatch");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// computeSpotCheckHash — FNV-1a of first, middle, and last lines combined
// ---------------------------------------------------------------------------
uint32_t DictionaryIndex::computeSpotCheckHash(HalFile& file, uint32_t fileSize) {
  char lineBuf[256];
  uint32_t hash = 2166136261u;
  BufferedReader reader(file);

  auto hashLine = [&](const char* line, int len) {
    for (int i = 0; i < len; ++i) {
      hash ^= static_cast<uint8_t>(line[i]);
      hash *= 16777619u;
    }
  };

  // First line
  reader.seekSet(0);
  int len = readLine(reader, lineBuf, sizeof(lineBuf));
  hashLine(lineBuf, len);

  // Middle line — seek to ~midpoint, skip partial line, read next full line
  if (fileSize > 2) {
    reader.seekSet(fileSize / 2);
    // Skip remainder of current (partial) line
    readLine(reader, lineBuf, sizeof(lineBuf));
    // Read next full line
    len = readLine(reader, lineBuf, sizeof(lineBuf));
    hashLine(lineBuf, len);
  }

  // Last line — seek near end, skip partial, read remaining
  if (fileSize > 2) {
    uint32_t nearEnd = fileSize > 512 ? fileSize - 512 : 0;
    reader.seekSet(nearEnd);
    // Skip partial line
    readLine(reader, lineBuf, sizeof(lineBuf));
    // Read lines until we get the last non-empty one
    char lastLine[256];
    lastLine[0] = '\0';
    int lastLen = 0;
    for (int guard = 0; guard < 10000; ++guard) {
      int n = readLine(reader, lineBuf, sizeof(lineBuf));
      if (n == 0 && reader.eof()) break;
      if (n > 0) {
        memcpy(lastLine, lineBuf, n + 1);  // include null
        lastLen = n;
      }
    }
    hashLine(lastLine, lastLen);
  }

  return hash;
}

// ---------------------------------------------------------------------------
// generateIndex — Two-pass: validate then write
// ---------------------------------------------------------------------------
bool DictionaryIndex::generateIndex(const char* dictPath, const char* idxPath, bool& outCorrupt) {
  outCorrupt = false;

  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) return false;

  uint32_t dictSize = static_cast<uint32_t>(dictFile.size());
  // Heap-allocate line buffer — 512 bytes exceeds 256-byte stack guideline
  auto* lineBuf = static_cast<char*>(malloc(512));
  if (!lineBuf) {
    LOG_ERR("DICT", "malloc failed for generateIndex line buffer");
    dictFile.close();
    return false;
  }
  static constexpr int LINE_BUF_SIZE = 512;
  char prevWord[DICT_WORD_MAX];
  prevWord[0] = '\0';

  BufferedReader reader(dictFile);

  // --- Pass 1: Count entries, verify sort order and tab separator ---
  uint32_t entryCount = 0;
  reader.seekSet(0);
  while (true) {
    int len = readLine(reader, lineBuf, LINE_BUF_SIZE);
    if (len == 0 && reader.eof()) break;
    if (len == 0) continue;  // skip blank lines

    // Find tab separator
    const char* tab = strchr(lineBuf, '\t');
    if (!tab) {
      LOG_ERR("DICT", "Line %u missing tab separator", entryCount + 1);
      outCorrupt = true;
      dictFile.close();
      free(lineBuf);
      return false;
    }

    // Extract word (truncate to DICT_WORD_MAX - 1)
    int wordLen = static_cast<int>(tab - lineBuf);
    if (wordLen >= DICT_WORD_MAX) wordLen = DICT_WORD_MAX - 1;
    char curWord[DICT_WORD_MAX];
    memcpy(curWord, lineBuf, wordLen);
    curWord[wordLen] = '\0';

    // Verify sort order (case-insensitive)
    if (prevWord[0] != '\0' && strcasecmp(curWord, prevWord) < 0) {
      LOG_ERR("DICT", "Sort violation: \"%s\" < \"%s\"", curWord, prevWord);
      outCorrupt = true;
      dictFile.close();
      free(lineBuf);
      return false;
    }
    memcpy(prevWord, curWord, wordLen + 1);
    entryCount++;
  }

  if (entryCount == 0) {
    LOG_ERR("DICT", "Dictionary is empty: %s", dictPath);
    outCorrupt = true;
    dictFile.close();
    free(lineBuf);
    return false;
  }

  // Compute spot-check hash before closing
  uint32_t spotHash = computeSpotCheckHash(dictFile, dictSize);
  dictFile.close();

  // --- Pass 2: Write index entries ---
  HalFile idxFile;
  if (!Storage.openFileForWrite("DICT", idxPath, idxFile)) {
    LOG_ERR("DICT", "Cannot open index for writing: %s", idxPath);
    free(lineBuf);
    return false;
  }

  // Write header
  DictIndexHeader header;
  header.dictFileSize = dictSize;
  header.spotCheckHash = spotHash;
  header.entryCount = entryCount;
  if (idxFile.write(&header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("DICT", "Failed to write index header");
    idxFile.close();
    free(lineBuf);
    return false;
  }

  // Re-open dict file for pass 2
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) {
    idxFile.close();
    free(lineBuf);
    return false;
  }
  BufferedReader reader2(dictFile);
  reader2.seekSet(0);

  for (uint32_t i = 0; i < entryCount; ++i) {
    // Record byte offset BEFORE reading the line
    uint32_t lineStart = reader2.position();

    int len = readLine(reader2, lineBuf, LINE_BUF_SIZE);
    // Skip blank lines (same logic as pass 1)
    while (len == 0 && !reader2.eof()) {
      lineStart = reader2.position();
      len = readLine(reader2, lineBuf, LINE_BUF_SIZE);
    }

    const char* tab = strchr(lineBuf, '\t');
    int wordLen = tab ? static_cast<int>(tab - lineBuf) : len;
    if (wordLen >= DICT_WORD_MAX) wordLen = DICT_WORD_MAX - 1;

    DictIndexEntry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.word, lineBuf, wordLen);
    entry.word[wordLen] = '\0';
    entry.byteOffset = lineStart;

    if (idxFile.write(&entry, sizeof(entry)) != sizeof(entry)) {
      LOG_ERR("DICT", "Failed to write index entry %u", i);
      dictFile.close();
      idxFile.close();
      free(lineBuf);
      return false;
    }
  }

  dictFile.close();
  idxFile.close();
  free(lineBuf);
  return true;
}

// ---------------------------------------------------------------------------
// binarySearchIndex
// ---------------------------------------------------------------------------
int32_t DictionaryIndex::binarySearchIndex(HalFile& idxFile, uint32_t entryCount, const char* word) {
  int32_t lo = 0;
  int32_t hi = static_cast<int32_t>(entryCount) - 1;
  int32_t bestMatch = -1;

  while (lo <= hi) {
    int32_t mid = lo + (hi - lo) / 2;
    size_t offset = DICT_INDEX_HEADER_SIZE + static_cast<size_t>(mid) * sizeof(DictIndexEntry);
    idxFile.seekSet(offset);

    DictIndexEntry entry;
    if (idxFile.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) {
      LOG_ERR("DICT", "Failed to read index entry at %d", mid);
      return -1;
    }
    entry.word[DICT_WORD_MAX - 1] = '\0';  // safety

    // Compare using prefix length (index words are truncated to DICT_WORD_MAX - 1)
    int cmp = strncasecmp(word, entry.word, DICT_WORD_MAX - 1);
    if (cmp == 0) {
      bestMatch = mid;
      hi = mid - 1;  // find first match (leftmost)
    } else if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }

  return bestMatch;
}

// ---------------------------------------------------------------------------
// lookup — binary search in index, then read definition from dict file
// ---------------------------------------------------------------------------
bool DictionaryIndex::lookup(const char* dictPath, const char* word, char* outDef, int outDefSize) {
  if (outDefSize < 4) {
    if (outDefSize > 0) outDef[0] = '\0';
    return false;
  }
  outDef[0] = '\0';

  char idxPath[128];
  if (!buildIdxPath(dictPath, idxPath, sizeof(idxPath))) return false;

  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", idxPath, idxFile)) return false;

  // Read header to get entry count
  DictIndexHeader header;
  if (idxFile.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    idxFile.close();
    return false;
  }

  int32_t idx = binarySearchIndex(idxFile, header.entryCount, word);
  if (idx < 0) {
    idxFile.close();
    return false;
  }

  // Open the dict file for full-word verification
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) {
    idxFile.close();
    return false;
  }

  static constexpr int LOOKUP_BUF_SIZE = 2560;
  auto* lineBuf = static_cast<char*>(malloc(LOOKUP_BUF_SIZE));
  if (!lineBuf) {
    LOG_ERR("DICT", "malloc failed for lookup buffer");
    dictFile.close();
    idxFile.close();
    return false;
  }

  // Walk adjacent entries sharing the same truncated prefix (handles words > 31 chars).
  // Limit iterations to avoid pathological cases with many shared prefixes.
  static constexpr int32_t MAX_PREFIX_WALK = 32;
  bool found = false;
  for (int32_t candidate = idx; candidate < static_cast<int32_t>(header.entryCount) && candidate < idx + MAX_PREFIX_WALK;
       ++candidate) {
    size_t entryOffset = DICT_INDEX_HEADER_SIZE + static_cast<size_t>(candidate) * sizeof(DictIndexEntry);
    idxFile.seekSet(entryOffset);
    DictIndexEntry entry;
    if (idxFile.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) break;
    entry.word[DICT_WORD_MAX - 1] = '\0';

    // Stop once prefix no longer matches
    if (strncasecmp(word, entry.word, DICT_WORD_MAX - 1) != 0) break;

    // Read the full line from .dict to verify full word match
    dictFile.seekSet(entry.byteOffset);
    int len = readLine(dictFile, lineBuf, LOOKUP_BUF_SIZE);
    if (len == 0) continue;

    const char* tab = strchr(lineBuf, '\t');
    if (!tab) continue;

    int lineWordLen = static_cast<int>(tab - lineBuf);
    char lineWord[256];
    if (lineWordLen >= static_cast<int>(sizeof(lineWord))) lineWordLen = static_cast<int>(sizeof(lineWord)) - 1;
    memcpy(lineWord, lineBuf, lineWordLen);
    lineWord[lineWordLen] = '\0';

    if (strcasecmp(word, lineWord) != 0) continue;

    // Match found — extract definition and unescape \\n sequences to real newlines
    const char* def = tab + 1;
    int defLen = len - static_cast<int>(def - lineBuf);

    if (defLen >= outDefSize) {
      memcpy(outDef, def, outDefSize - 4);
      memcpy(outDef + outDefSize - 4, "...", 4);  // includes null
    } else {
      memcpy(outDef, def, defLen + 1);  // includes null
    }

    // In-place unescape: convert literal "\n" (two chars) to newline (one char)
    int r = 0, w = 0;
    while (outDef[r] != '\0') {
      if (outDef[r] == '\\' && outDef[r + 1] == 'n') {
        outDef[w++] = '\n';
        r += 2;
      } else {
        outDef[w++] = outDef[r++];
      }
    }
    outDef[w] = '\0';

    found = true;
    break;
  }

  free(lineBuf);
  dictFile.close();
  idxFile.close();
  return found;
}
