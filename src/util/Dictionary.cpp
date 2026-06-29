#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "StringUtils.h"
#include "TextPool.h"

// Static member definitions
char Dictionary::wordBuf[256] = "";

namespace {
constexpr char DICT_BIN[] = "dictionary.bin";
constexpr char GLOBAL_DICT_DIR[] = "/.crosspoint";
constexpr int MAX_DICT_PATH_BYTES = 512;
}  // namespace

// .idx.fpi / .syn.fpi file constants — the Fenced Prefix Index sidecar. It narrows a
// lookup to a short byte range mostly by reading sidecar sectors instead of .idx/.syn
// sectors, minimizing slow SD sector reads; see the "Fenced Prefix Index" section of
// docs/dictionary-development.md for the motivation and the benchmark that picked
// this tuning (fencep_6 + tab1_3) over the other candidates tried.
// No self-description beyond a version byte — prefix lengths are fixed constants,
// mirrored exactly in scripts/dictionary_tools.py and test/data/generate_dictionaries.py.
//
// Sector 0 (512 bytes): 1-byte version + a "tab" table of FPI_TAB_ENTRY_CAP 3-byte
// lowercase prefixes (zero-padded), evenly sampled across source-file 512B sectors.
// Sectors 1..N: a "fencep" sidecar — front-coded (prefix-compressed) first-word
// prefixes for every source-file 512B sector, grouped 8 sectors at a time. Each group
// also carries a 4-byte LE cumulative ordinal (the 0-based entry index of the group's
// head (first) sector's fence word), enabling binarySearchFpiOrdinal to resolve a
// word ordinal to a byte offset via direct random-access seeks, without decoding any
// front-coded prefixes.
static constexpr uint32_t FPI_SECTOR_SIZE = 512;
static constexpr uint8_t FPI_VERSION = 0;
static constexpr uint32_t FPI_HEADER_SIZE = 1;
static constexpr uint8_t FPI_TAB_PREFIX_LEN = 3;
static constexpr uint32_t FPI_TAB_ENTRY_CAP = (FPI_SECTOR_SIZE - FPI_HEADER_SIZE) / FPI_TAB_PREFIX_LEN;  // 170
static constexpr uint8_t FPI_FENCEP_PREFIX_LEN = 6;
static constexpr uint32_t FPI_GROUP_SECTORS = 8;  // source-file sectors covered per fencep group
// Byte offset within a group of the trailing 4-byte LE cumulative-ordinal field.
static constexpr uint32_t FPI_GROUP_ORDINAL_OFFSET = 9 + FPI_GROUP_SECTORS * FPI_FENCEP_PREFIX_LEN;  // 57
static constexpr uint32_t FPI_ORDINAL_SIZE = 4;
static constexpr uint32_t FPI_GROUP_SIZE = FPI_GROUP_ORDINAL_OFFSET + FPI_ORDINAL_SIZE;      // 61 bytes
static constexpr uint32_t FPI_GROUPS_PER_SIDECAR_SECTOR = FPI_SECTOR_SIZE / FPI_GROUP_SIZE;  // 8
// Source-file sectors described by one 512B fencep sidecar sector.
static constexpr uint32_t FPI_SRC_SECTORS_PER_SIDECAR_SECTOR = FPI_GROUPS_PER_SIDECAR_SECTOR * FPI_GROUP_SECTORS;  // 64
// Front-coded common-prefix-length field is 7 bits (top bit is the front-coding flag).
static constexpr uint32_t FPI_MAX_COMMON_LEN = 127;
// Bound on the reconstructed "previous word" scratch used for front-coding within a
// group: each step's length is capped at FPI_MAX_COMMON_LEN + (prefixLen-1) bytes.
static constexpr uint32_t FPI_PREV_WORD_CAP = FPI_MAX_COMMON_LEN + FPI_FENCEP_PREFIX_LEN;  // 133, rounded up below

// ---------------------------------------------------------------------------
// Path management
// ---------------------------------------------------------------------------

std::string Dictionary::readDictPath(const char* cachePath) {
  char binPath[128];

  // Try per-book dictionary.bin first when cachePath is provided.
  if (cachePath && cachePath[0] != '\0') {
    snprintf(binPath, sizeof(binPath), "%s/%s", cachePath, DICT_BIN);
    HalFile f;
    if (Storage.openFileForRead("DICT", binPath, f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0 && sz <= MAX_DICT_PATH_BYTES) {
        std::string result(sz, '\0');
        const int n = f.read(&result[0], sz);
        f.close();
        if (n > 0) {
          result.resize(static_cast<size_t>(n));
          return result;
        }
      } else {
        f.close();
      }
    }
    // Per-book file absent or empty ("Use Global") — fall through to global.
  }

  // Read global dictionary.bin.
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForRead("DICT", binPath, f)) return "";
  const int sz = static_cast<int>(f.fileSize());
  if (sz <= 0 || sz > MAX_DICT_PATH_BYTES) {
    f.close();
    return "";
  }
  std::string result(sz, '\0');
  const int n = f.read(&result[0], sz);
  f.close();
  if (n <= 0) return "";
  result.resize(static_cast<size_t>(n));
  return result;
}

void Dictionary::saveGlobalDictPath(const char* folderPath) {
  char binPath[128];
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForWrite("DICT", binPath, f)) {
    LOG_ERR("DICT", "Could not write global dictionary path");
    return;
  }
  if (folderPath && folderPath[0] != '\0') {
    f.write(reinterpret_cast<const uint8_t*>(folderPath), strlen(folderPath));
  }
  f.close();
}

// ---------------------------------------------------------------------------
// Validity checks
// ---------------------------------------------------------------------------

bool Dictionary::exists(const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  if (!Storage.exists(dp.idx().c_str())) return false;
  return Storage.exists(dp.dict().c_str());
}

bool Dictionary::isValidDictionary() {
  std::string folderPath = readDictPath(nullptr);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  const bool idxExists = Storage.exists(dp.idx().c_str());
  const bool valid = idxExists && Storage.exists(dp.dict().c_str());
  if (!valid) {
    LOG_DBG("DICT", "Stored dictionary path no longer valid, resetting");
    saveGlobalDictPath("");
  }
  return valid;
}

// ---------------------------------------------------------------------------
// .ifo parsing
// ---------------------------------------------------------------------------

DictInfo Dictionary::readInfo(const char* folderPath) {
  DictInfo info;
  if (folderPath == nullptr || folderPath[0] == '\0') return info;

  std::string folder(folderPath);
  std::string ifoPath = DictPaths(folder).ifo();

  HalFile file;
  if (!Storage.openFileForRead("DICT", ifoPath.c_str(), file)) return info;

  // Validate header line byte by byte — no line buffer needed.
  static constexpr const char HEADER[] = "StarDict's dict ifo file";
  for (size_t i = 0; i < sizeof(HEADER) - 1; i++) {
    int b = file.read();
    if (b < 0 || static_cast<char>(b) != HEADER[i]) {
      LOG_ERR("DICT", "Invalid .ifo header in %s", folderPath);
      file.close();
      return info;
    }
  }
  // Skip remainder of header line.
  {
    int b;
    while ((b = file.read()) >= 0 && b != '\n') {
    }
  }

  // Serial key=value parse. State fits in ~50 bytes vs the old 512-byte slurp buffer.
  char keyBuf[24];  // longest key: "sametypesequence" = 16 chars
  int keyLen = 0;
  bool readingVal = false;
  char* valDst = nullptr;
  size_t valCap = 0;
  size_t valWritten = 0;
  bool isNumField = false;
  uint32_t* valNum = nullptr;
  uint32_t numAccum = 0;

  while (file.available()) {
    int b = file.read();
    if (b < 0) break;
    const char c = static_cast<char>(b);

    if (c == '\r') continue;

    if (!readingVal) {
      if (c == '\n') {
        keyLen = 0;
      } else if (c == '=') {
        keyBuf[keyLen] = '\0';
        keyLen = 0;
        valDst = nullptr;
        valCap = 0;
        valWritten = 0;
        isNumField = false;
        valNum = nullptr;
        numAccum = 0;
        if (strcmp(keyBuf, "bookname") == 0) {
          valDst = info.bookname;
          valCap = sizeof(info.bookname) - 1;
        } else if (strcmp(keyBuf, "sametypesequence") == 0) {
          valDst = info.sametypesequence;
          valCap = sizeof(info.sametypesequence) - 1;
        } else if (strcmp(keyBuf, "website") == 0) {
          valDst = info.website;
          valCap = sizeof(info.website) - 1;
        } else if (strcmp(keyBuf, "date") == 0) {
          valDst = info.date;
          valCap = sizeof(info.date) - 1;
        } else if (strcmp(keyBuf, "description") == 0) {
          valDst = info.description;
          valCap = sizeof(info.description) - 1;
        } else if (strcmp(keyBuf, "lang") == 0) {
          valDst = info.lang;
          valCap = sizeof(info.lang) - 1;
        } else if (strcmp(keyBuf, "wordcount") == 0) {
          isNumField = true;
          valNum = &info.wordcount;
        } else if (strcmp(keyBuf, "idxfilesize") == 0) {
          isNumField = true;
          valNum = &info.idxfilesize;
        }
        readingVal = true;
      } else if (keyLen < static_cast<int>(sizeof(keyBuf) - 1)) {
        keyBuf[keyLen++] = c;
      }
    } else {
      if (c == '\n') {
        if (valDst) valDst[valWritten] = '\0';
        if (isNumField && valNum) *valNum = numAccum;
        readingVal = false;
        keyLen = 0;
      } else if (isNumField) {
        if (c >= '0' && c <= '9') numAccum = numAccum * 10 + static_cast<uint32_t>(c - '0');
      } else if (valDst && valWritten < valCap) {
        valDst[valWritten++] = c;
      }
    }
  }

  file.close();

  DictPaths dp(folder);
  const bool dictExists = Storage.exists(dp.dict().c_str());
  info.isCompressed = !dictExists && Storage.exists(dp.dictDz().c_str());

  info.valid = true;
  return info;
}

// ---------------------------------------------------------------------------
// Word cleaning
// ---------------------------------------------------------------------------

std::string Dictionary::cleanWord(const std::string& word) {
  if (word.empty()) return "";

  size_t start = 0;
  while (start < word.size() && !std::isalnum(static_cast<unsigned char>(word[start]))) {
    start++;
  }

  size_t end = word.size();
  while (end > start && !std::isalnum(static_cast<unsigned char>(word[end - 1]))) {
    end--;
  }

  if (start >= end) return "";

  std::string result = word.substr(start, end - start);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
  return result;
}

// ---------------------------------------------------------------------------
// Low-level file reading helpers
// ---------------------------------------------------------------------------

int Dictionary::readWordInto(HalFile& file, char* buf, size_t bufSize) {
  size_t i = 0;
  while (i < bufSize - 1) {
    int ch = file.read();
    if (ch < 0) return -1;  // EOF or I/O error
    if (ch == 0) {
      buf[i] = '\0';
      return static_cast<int>(i);
    }
    buf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync
  buf[bufSize - 1] = '\0';
  int ch;
  do {
    ch = file.read();
  } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

// ---------------------------------------------------------------------------
// .fpi (Fenced Prefix Index) generation and lookup
// ---------------------------------------------------------------------------

namespace {

uint32_t fpiCeilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Linear-interpolation sample point used by both the writer (which source-file
// sector does tab slot `i` sample?) and the reader (which sector does a tab
// neighbour index correspond to?). Port of dict_index_benchmark.go's sampledSector.
uint32_t fpiSampledSector(uint32_t i, uint32_t entryCount, uint32_t sectorCount) {
  if (entryCount <= 1 || sectorCount <= 1) return 0;
  return static_cast<uint32_t>((static_cast<uint64_t>(i) * (sectorCount - 1)) / (entryCount - 1));
}

// Number of leading bytes that match between a and b, up to min(aLen, bLen).
uint32_t fpiCommonPrefixLen(const uint8_t* a, uint32_t aLen, const uint8_t* b, uint32_t bLen) {
  const uint32_t n = aLen < bLen ? aLen : bLen;
  uint32_t i = 0;
  while (i < n && a[i] == b[i]) i++;
  return i;
}

}  // namespace

bool Dictionary::generateFpi(const char* srcPath, const char* fpiPath, uint8_t skipPerEntry,
                             void (*onProgress)(void*, uint32_t, uint32_t), bool (*shouldCancel)(void*), void* ctx) {
  HalFile src;
  if (!Storage.openFileForRead("DICT_FPI", srcPath, src)) {
    LOG_ERR("DICT_FPI", "Failed to open: %s", srcPath);
    return false;
  }

  const uint32_t srcFileSize = static_cast<uint32_t>(src.fileSize());
  const uint32_t sectorCount = srcFileSize == 0 ? 0 : fpiCeilDiv(srcFileSize, FPI_SECTOR_SIZE);
  const uint32_t tabEntryCount = sectorCount < FPI_TAB_ENTRY_CAP ? sectorCount : FPI_TAB_ENTRY_CAP;

  HalFile out;
  if (!Storage.openFileForWrite("DICT_FPI", fpiPath, out)) {
    src.close();
    LOG_ERR("DICT_FPI", "Failed to open for write: %s", fpiPath);
    return false;
  }

  // Tab table (511 bytes) is only fully known once the whole pass completes, so it's
  // accumulated in RAM and patched into sector 0 at the end (same "reserve, patch
  // later" idiom used for headers). Heap-allocated because 511 bytes
  // exceeds the project's <256B local-stack-variable budget; freed automatically on
  // return via unique_ptr.
  auto tabBuf = makeUniqueNoThrow<uint8_t[]>(FPI_SECTOR_SIZE - FPI_HEADER_SIZE);
  if (!tabBuf) {
    LOG_ERR("DICT_FPI", "OOM: tab buffer (%u bytes)", FPI_SECTOR_SIZE - FPI_HEADER_SIZE);
    src.close();
    out.close();
    return false;
  }
  memset(tabBuf.get(), 0, FPI_SECTOR_SIZE - FPI_HEADER_SIZE);

  // Reserve sector 0 as a zero placeholder. The version byte is already correct
  // (FPI_VERSION == 0); only the tab bytes need patching in later.
  {
    const uint8_t versionByte = FPI_VERSION;
    if (out.write(&versionByte, 1) != 1 || out.write(tabBuf.get(), FPI_SECTOR_SIZE - FPI_HEADER_SIZE) !=
                                               static_cast<int>(FPI_SECTOR_SIZE - FPI_HEADER_SIZE)) {
      LOG_ERR("DICT_FPI", "Placeholder sector0 write failed: %s", fpiPath);
      src.close();
      out.close();
      return false;
    }
  }

  uint32_t nextTabIdx = 0;
  uint32_t nextTabSector = tabEntryCount > 0 ? fpiSampledSector(0, tabEntryCount, sectorCount) : UINT32_MAX;

  uint8_t group[FPI_GROUP_SIZE] = {};
  uint8_t prevWord[FPI_PREV_WORD_CAP] = {};
  uint32_t prevWordLen = 0;
  uint32_t groupsInSidecarSector = 0;
  bool groupPending = false;
  bool error = false;

  // Encode one source-file sector's fence word into the group buffer (front-coded
  // against the previous local sector's word within the same 8-sector group), sample
  // it into the tab table if it's a sample point, and flush the group/sidecar-sector
  // once full. word/wordLen must already be lowercased. ordinal is the 0-based entry
  // index of `word` in srcPath; it's only persisted for the group's head sector
  // (one 4-byte LE cumulative-ordinal field per group, consumed by binarySearchFpiOrdinal).
  auto assignFence = [&](uint32_t sector, const char* word, uint32_t wordLen, uint32_t rel, uint32_t ordinal) -> bool {
    const uint32_t local = sector % FPI_GROUP_SECTORS;
    if (local == 0) {
      memset(group, 0, FPI_GROUP_SIZE);
      prevWordLen = 0;
      uint32_t leOrdinal = ordinal;  // ESP32-C3 is little-endian; memcpy for portability/alignment safety.
      memcpy(group + FPI_GROUP_ORDINAL_OFFSET, &leOrdinal, FPI_ORDINAL_SIZE);
    }
    const uint32_t rel9 = rel & 0x1ff;
    group[local] = static_cast<uint8_t>(rel9 & 0xff);
    if (rel9 & 0x100) group[8] |= static_cast<uint8_t>(1u << local);

    uint8_t* slot = group + 9 + local * FPI_FENCEP_PREFIX_LEN;
    uint32_t common = 0;
    if (local != 0) {
      common = fpiCommonPrefixLen(prevWord, prevWordLen, reinterpret_cast<const uint8_t*>(word), wordLen);
      if (common > FPI_MAX_COMMON_LEN) common = FPI_MAX_COMMON_LEN;
    }
    if (local != 0 && common > 0) {
      slot[0] = static_cast<uint8_t>(0x80 | common);
      for (uint32_t i = 0; i < FPI_FENCEP_PREFIX_LEN - 1; i++) {
        const uint32_t srcIdx = common + i;
        slot[1 + i] = srcIdx < wordLen ? static_cast<uint8_t>(word[srcIdx]) : 0;
      }
      memcpy(prevWord + common, slot + 1, FPI_FENCEP_PREFIX_LEN - 1);
      prevWordLen = common + (FPI_FENCEP_PREFIX_LEN - 1);
    } else {
      for (uint32_t i = 0; i < FPI_FENCEP_PREFIX_LEN; i++) {
        slot[i] = i < wordLen ? static_cast<uint8_t>(word[i]) : 0;
      }
      memcpy(prevWord, slot, FPI_FENCEP_PREFIX_LEN);
      prevWordLen = FPI_FENCEP_PREFIX_LEN;
    }

    uint8_t* const tabBufPtr = tabBuf.get();
    while (nextTabIdx < tabEntryCount && sector == nextTabSector) {
      uint8_t* tabSlot = tabBufPtr + nextTabIdx * FPI_TAB_PREFIX_LEN;
      for (uint32_t i = 0; i < FPI_TAB_PREFIX_LEN; i++) tabSlot[i] = i < wordLen ? static_cast<uint8_t>(word[i]) : 0;
      nextTabIdx++;
      nextTabSector =
          nextTabIdx < tabEntryCount ? fpiSampledSector(nextTabIdx, tabEntryCount, sectorCount) : UINT32_MAX;
    }

    groupPending = true;
    if (local == FPI_GROUP_SECTORS - 1) {
      if (out.write(group, FPI_GROUP_SIZE) != static_cast<int>(FPI_GROUP_SIZE)) return false;
      groupPending = false;
      groupsInSidecarSector++;
      if (groupsInSidecarSector == FPI_GROUPS_PER_SIDECAR_SECTOR) {
        constexpr uint32_t PAD = FPI_SECTOR_SIZE - FPI_GROUPS_PER_SIDECAR_SECTOR * FPI_GROUP_SIZE;  // 24
        const uint8_t pad[PAD] = {};
        if (out.write(pad, PAD) != static_cast<int>(PAD)) return false;
        groupsInSidecarSector = 0;
      }
    }
    return true;
  };

  uint32_t nextSector = 0;
  uint32_t cumulativeOrdinal = 0;  // 0-based index of the entry currently being parsed
  uint8_t skipBuf[8];
  constexpr uint32_t PROGRESS_INTERVAL = 65536;
  uint32_t lastProgressPos = 0;

  while (true) {
    const uint32_t entryPos = static_cast<uint32_t>(src.position());
    if (entryPos >= srcFileSize) break;

    int wordLen = readWordInto(src, wordBuf, sizeof(wordBuf));
    if (wordLen < 0) {
      LOG_ERR("DICT_FPI", "Unexpected EOF in %s", srcPath);
      error = true;
      break;
    }
    if (src.read(skipBuf, skipPerEntry) != static_cast<int>(skipPerEntry)) {
      LOG_ERR("DICT_FPI", "Truncated entry suffix in %s", srcPath);
      error = true;
      break;
    }

    // Lowercase in place (shared scratch buffer — no new stack allocation).
    for (int i = 0; i < wordLen; i++) {
      wordBuf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(wordBuf[i])));
    }

    // A single entry can be the fence word for multiple consecutive sectors if
    // earlier sectors have no entry starting within their own bounds (mirrors
    // buildSectorFences's sort.Search in the Go reference). All such sectors share
    // this entry's ordinal — only a group's head sector actually persists it.
    while (nextSector < sectorCount && static_cast<uint64_t>(nextSector) * FPI_SECTOR_SIZE <= entryPos) {
      const uint32_t rel = entryPos - nextSector * FPI_SECTOR_SIZE;
      if (!assignFence(nextSector, wordBuf, static_cast<uint32_t>(wordLen), rel, cumulativeOrdinal)) {
        error = true;
        break;
      }
      nextSector++;
    }
    if (error) break;
    cumulativeOrdinal++;

    const uint32_t pos = static_cast<uint32_t>(src.position());
    if (pos - lastProgressPos >= PROGRESS_INTERVAL) {
      lastProgressPos = pos;
      if (onProgress) onProgress(ctx, pos, srcFileSize);
      if (shouldCancel && shouldCancel(ctx)) {
        error = true;
        break;
      }
    }
  }

  // Trailing sectors with no entry starting inside them (rare — only possible past
  // the last entry) get an empty fence, matching the Go reference's zero-value default.
  // Their ordinal is the sentinel cumulativeOrdinal (== total entry count), a valid
  // upper bound for binarySearchFpiOrdinal.
  while (!error && nextSector < sectorCount) {
    if (!assignFence(nextSector, "", 0, 0, cumulativeOrdinal)) error = true;
    nextSector++;
  }

  // Flush a partial trailing group and pad its sidecar sector.
  if (!error && groupPending) {
    if (out.write(group, FPI_GROUP_SIZE) != static_cast<int>(FPI_GROUP_SIZE)) {
      error = true;
    } else {
      groupsInSidecarSector++;
    }
  }
  if (!error && groupsInSidecarSector > 0 && groupsInSidecarSector < FPI_GROUPS_PER_SIDECAR_SECTOR) {
    uint32_t pad = FPI_SECTOR_SIZE - groupsInSidecarSector * FPI_GROUP_SIZE;
    const uint8_t zeroChunk[FPI_GROUP_SIZE] = {};
    while (pad > 0 && !error) {
      const uint32_t n = pad < FPI_GROUP_SIZE ? pad : FPI_GROUP_SIZE;
      if (out.write(zeroChunk, n) != static_cast<size_t>(n)) error = true;
      pad -= n;
    }
  }

  if (!error) {
    out.seekSet(0);
    const uint8_t versionByte = FPI_VERSION;
    if (out.write(&versionByte, 1) != 1 || out.write(tabBuf.get(), FPI_SECTOR_SIZE - FPI_HEADER_SIZE) !=
                                               static_cast<int>(FPI_SECTOR_SIZE - FPI_HEADER_SIZE)) {
      LOG_ERR("DICT_FPI", "Sector0 patch write failed: %s", fpiPath);
      error = true;
    }
  }

  src.close();
  out.close();

  if (error) {
    LOG_ERR("DICT_FPI", "Generation failed: %s", fpiPath);
    return false;
  }
  if (onProgress) onProgress(ctx, srcFileSize, srcFileSize);
  return true;
}

namespace {

uint32_t fpiReadBe32(const uint8_t* b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

int fpiAsciiCmpCI(const uint8_t* a, uint32_t aLen, const uint8_t* b, uint32_t bLen) {
  const uint32_t n = aLen < bLen ? aLen : bLen;
  for (uint32_t i = 0; i < n; i++) {
    const int ta = std::tolower(a[i]);
    const int tb = std::tolower(b[i]);
    if (ta != tb) return ta < tb ? -1 : 1;
  }
  if (aLen < bLen) return -1;
  if (aLen > bLen) return 1;
  return 0;
}

// Absolute file offset of group g's first byte (sector 0 is the tab table; groups
// are packed FPI_GROUPS_PER_SIDECAR_SECTOR at a time into each subsequent sidecar
// sector, with a few padding bytes trailing each full sidecar sector).
uint32_t fpiGroupFileOffset(uint32_t g) {
  const uint32_t sidecarSector = g / FPI_GROUPS_PER_SIDECAR_SECTOR;
  const uint32_t groupInSector = g % FPI_GROUPS_PER_SIDECAR_SECTOR;
  return FPI_SECTOR_SIZE + sidecarSector * FPI_SECTOR_SIZE + groupInSector * FPI_GROUP_SIZE;
}

// Port of dict_index_benchmark.go's tabPrefixCmp: a shorter target that's a
// case-insensitive prefix of the (longer) stored tab entry compares as equal — the
// entry is ambiguous (could represent any word starting with target), so the search
// must not rule it out in either direction.
int fpiTabCmp(const uint8_t* e, const uint8_t* target, uint32_t targetLen) {
  const uint8_t* p = static_cast<const uint8_t*>(std::memchr(e, 0, FPI_TAB_PREFIX_LEN));
  const uint32_t len = p ? static_cast<uint32_t>(p - e) : FPI_TAB_PREFIX_LEN;
  if (targetLen < len && fpiAsciiCmpCI(e, targetLen, target, targetLen) == 0) return 0;
  return fpiAsciiCmpCI(e, len, target, targetLen);
}

// Reconstruct a sector's fence byte offset from its group entry.
uint32_t fpiResolveSectorOffset(HalFile& fpi, uint32_t sector, uint32_t fallback) {
  const uint32_t groupIndex = sector / FPI_GROUP_SECTORS;
  const uint32_t local = sector % FPI_GROUP_SECTORS;
  fpi.seekSet(fpiGroupFileOffset(groupIndex));
  uint8_t group[FPI_GROUP_SIZE];
  if (fpi.read(group, FPI_GROUP_SIZE) != static_cast<int>(FPI_GROUP_SIZE)) {
    return fallback;
  }
  uint32_t rel = group[local];
  if (group[8] & (1u << local)) rel |= 0x100;
  return sector * FPI_SECTOR_SIZE + rel;
}

// Read group's head ordinal (its first sector's ordinal) from its cumulative-ordinal field.
uint32_t fpiReadGroupOrdinal(HalFile& fpi, uint32_t g) {
  uint8_t raw[4];
  fpi.seekSet(fpiGroupFileOffset(g) + FPI_GROUP_ORDINAL_OFFSET);
  if (fpi.read(raw, 4) != 4) return UINT32_MAX;
  uint32_t val;
  memcpy(&val, raw, 4);
  return val;
}

// Decode one already-read fencep sidecar sector and narrow [*start, *end) using the
// same "confirmed-complete vs. ambiguous-prefix" logic as dict_index_benchmark.go's
// narrowFencePCBounds/readFencePCBlock. baseSector is the source-file sector index of
// this sidecar sector's first group's head entry. Front-coding is reconstructed
// sequentially from each group's start, so this always decodes every entry in the
// sidecar sector up to sectorCount (or until a definitive upper bound is found).
void fpiNarrowFromSidecarSector(const uint8_t* raw, uint32_t sectorCount, uint32_t baseSector, const uint8_t* target,
                                uint32_t targetLen, uint32_t* start, uint32_t* end) {
  uint8_t prevWord[FPI_PREV_WORD_CAP];
  uint32_t prevWordLen = 0;
  bool haveLastLe = false;
  uint32_t lastLeStart = 0;

  for (uint32_t groupInSector = 0; groupInSector < FPI_GROUPS_PER_SIDECAR_SECTOR; groupInSector++) {
    const uint8_t* group = raw + groupInSector * FPI_GROUP_SIZE;
    for (uint32_t local = 0; local < FPI_GROUP_SECTORS; local++) {
      const uint32_t sector = baseSector + groupInSector * FPI_GROUP_SECTORS + local;
      if (sector >= sectorCount) {
        if (haveLastLe && lastLeStart > *start) *start = lastLeStart;
        return;
      }

      if (local == 0) prevWordLen = 0;

      uint32_t rel = group[local];
      if (group[8] & (1u << local)) rel |= 0x100;
      const uint32_t entryStart = sector * FPI_SECTOR_SIZE + rel;

      const uint8_t* slot = group + 9 + local * FPI_FENCEP_PREFIX_LEN;
      if (local != 0 && (slot[0] & 0x80)) {
        const uint32_t common = slot[0] & 0x7f;
        const uint32_t copyCommon = common < prevWordLen ? common : prevWordLen;
        // prevWord[0..copyCommon) is already correct; append the new suffix in place.
        memcpy(prevWord + copyCommon, slot + 1, FPI_FENCEP_PREFIX_LEN - 1);
        prevWordLen = copyCommon + (FPI_FENCEP_PREFIX_LEN - 1);
      } else {
        memcpy(prevWord, slot, FPI_FENCEP_PREFIX_LEN);
        prevWordLen = FPI_FENCEP_PREFIX_LEN;
      }

      // Confirmed-complete: an embedded zero byte terminates the real word early.
      uint32_t realLen = prevWordLen;
      bool confirmed = false;
      for (uint32_t i = 0; i < prevWordLen; i++) {
        if (prevWord[i] == 0) {
          realLen = i;
          confirmed = true;
          break;
        }
      }

      bool isGt = false;
      if (confirmed) {
        if (fpiAsciiCmpCI(prevWord, realLen, target, targetLen) <= 0) {
          lastLeStart = entryStart;
          haveLastLe = true;
        } else {
          isGt = true;
        }
      } else if (targetLen >= realLen && fpiAsciiCmpCI(target, realLen, prevWord, realLen) == 0) {
        // target starts with this (incomplete) prefix — ambiguous, skip (matches the
        // Go reference's explicit no-op for this case).
      } else if (fpiAsciiCmpCI(prevWord, realLen, target, targetLen) < 0) {
        lastLeStart = entryStart;
        haveLastLe = true;
      } else {
        isGt = true;
      }

      if (isGt) {
        if (haveLastLe && lastLeStart > *start) *start = lastLeStart;
        if (entryStart < *end) *end = entryStart;
        return;
      }
    }
  }
  if (haveLastLe && lastLeStart > *start) *start = lastLeStart;
}

}  // namespace

bool Dictionary::binarySearchFpi(HalFile& fpi, const char* target, uint32_t srcFileSize, uint32_t* startByte,
                                 uint32_t* endByte, uint32_t sectorMargin) {
  const uint32_t sectorCount = srcFileSize == 0 ? 0 : fpiCeilDiv(srcFileSize, FPI_SECTOR_SIZE);
  if (sectorCount == 0) return false;
  const uint32_t tabEntryCount = sectorCount < FPI_TAB_ENTRY_CAP ? sectorCount : FPI_TAB_ENTRY_CAP;
  const uint32_t targetLen = static_cast<uint32_t>(strlen(target));
  const uint8_t* targetBytes = reinterpret_cast<const uint8_t*>(target);

  // One 512-byte scratch buffer, reused for sector 0 and then for each fencep sidecar
  // sector read during narrowing (heap — exceeds the <256B stack-local budget).
  auto sectorBuf = makeUniqueNoThrow<uint8_t[]>(FPI_SECTOR_SIZE);
  if (!sectorBuf) {
    LOG_ERR("DICT_FPI", "OOM: fpi sector scratch (%u bytes)", FPI_SECTOR_SIZE);
    return false;
  }

  fpi.seekSet(0);
  if (fpi.read(sectorBuf.get(), FPI_SECTOR_SIZE) != static_cast<int>(FPI_SECTOR_SIZE)) return false;
  if (sectorBuf[0] != FPI_VERSION) return false;

  const uint8_t* const sectorBufPtr = sectorBuf.get();
  const uint8_t* tabBase = sectorBufPtr + FPI_HEADER_SIZE;

  // Tab comparisons always use target truncated to FPI_TAB_PREFIX_LEN (mirrors Go's
  // `targetPrefix := prefix(target, len(entries[0].prefix))`, computed once and reused
  // for the initial search AND both group-widening loops below). Without this
  // truncation, a target longer than a tab entry never compares as fpiTabPrefixCmp's
  // ambiguous "==0" case (it falls through to the length tiebreak instead), which
  // silently defeats the widening loops when most entries share the same prefix.
  const uint32_t tabTargetLen = targetLen < FPI_TAB_PREFIX_LEN ? targetLen : FPI_TAB_PREFIX_LEN;

  // Binary search: find the last tab entry whose prefix is <= target.
  uint32_t loEntry = 0, hiEntry = tabEntryCount - 1;
  while (loEntry < hiEntry) {
    const uint32_t mid = loEntry + (hiEntry - loEntry + 1) / 2;
    const uint8_t* e = tabBase + mid * FPI_TAB_PREFIX_LEN;
    if (fpiTabCmp(e, targetBytes, tabTargetLen) > 0) {
      hiEntry = mid - 1;
    } else {
      loEntry = mid;
    }
  }

  // Widen to the full run of ambiguous (cmp==0) neighbours, then bound with the
  // immediate non-equal entries on both sides — mirrors locateTabBounds exactly.
  uint32_t groupLo = loEntry;
  while (groupLo > 0) {
    const uint8_t* e = tabBase + (groupLo - 1) * FPI_TAB_PREFIX_LEN;
    if (fpiTabCmp(e, targetBytes, tabTargetLen) != 0) break;
    groupLo--;
  }
  uint32_t groupHi = loEntry;
  while (groupHi + 1 < tabEntryCount) {
    const uint8_t* e = tabBase + (groupHi + 1) * FPI_TAB_PREFIX_LEN;
    if (fpiTabCmp(e, targetBytes, tabTargetLen) != 0) break;
    groupHi++;
  }

  uint32_t loSector = groupLo > 0 ? fpiSampledSector(groupLo - 1, tabEntryCount, sectorCount) : 0;
  uint32_t hiSector =
      groupHi + 1 < tabEntryCount ? fpiSampledSector(groupHi + 1, tabEntryCount, sectorCount) : sectorCount - 1;
  if (hiSector < loSector) hiSector = loSector;

  uint32_t start = loSector * FPI_SECTOR_SIZE;
  uint32_t end = hiSector + 1 < sectorCount ? (hiSector + 1) * FPI_SECTOR_SIZE : srcFileSize;
  if (end > srcFileSize) end = srcFileSize;

  // Fencep narrowing: iteratively read the sidecar sector covering the midpoint
  // source-sector and tighten [start, end) until it collapses to one source sector,
  // no further progress is possible, or a read fails.
  int lastSidecarSector = -1;
  while (end > start) {
    const uint32_t lo = start / FPI_SECTOR_SIZE;
    const uint32_t hi = (end - 1) / FPI_SECTOR_SIZE;
    if (lo >= hi) break;

    const uint32_t oldStart = start, oldEnd = end;
    const uint32_t mid = lo + (hi - lo + 1) / 2;
    const uint32_t sidecarSector = mid / FPI_SRC_SECTORS_PER_SIDECAR_SECTOR;

    if (static_cast<int>(sidecarSector) != lastSidecarSector) {
      const uint32_t fileOffset = FPI_SECTOR_SIZE + sidecarSector * FPI_SECTOR_SIZE;
      fpi.seekSet(fileOffset);
      if (fpi.read(sectorBuf.get(), FPI_SECTOR_SIZE) != static_cast<int>(FPI_SECTOR_SIZE)) break;
      lastSidecarSector = static_cast<int>(sidecarSector);
    }

    const uint32_t baseSector = sidecarSector * FPI_SRC_SECTORS_PER_SIDECAR_SECTOR;
    fpiNarrowFromSidecarSector(sectorBuf.get(), sectorCount, baseSector, targetBytes, targetLen, &start, &end);

    if (start == oldStart && end == oldEnd) break;
    if (start / FPI_SECTOR_SIZE / FPI_SRC_SECTORS_PER_SIDECAR_SECTOR == sidecarSector &&
        (end - 1) / FPI_SECTOR_SIZE / FPI_SRC_SECTORS_PER_SIDECAR_SECTOR == sidecarSector) {
      break;
    }
  }

  if (end <= start) return false;

  if (sectorMargin > 0) {
    const uint32_t matchedSector = start / FPI_SECTOR_SIZE;
    const uint32_t matchedEndSector = (end - 1) / FPI_SECTOR_SIZE;

    const uint32_t startSector = (matchedSector > sectorMargin) ? (matchedSector - sectorMargin) : 0;
    const uint32_t endSector = matchedEndSector + sectorMargin;

    start = (startSector == 0) ? 0 : fpiResolveSectorOffset(fpi, startSector, 0);

    const uint32_t nextSector = endSector + 1;
    const uint32_t totalSectors = srcFileSize == 0 ? 0 : fpiCeilDiv(srcFileSize, FPI_SECTOR_SIZE);
    end = (nextSector >= totalSectors) ? srcFileSize : fpiResolveSectorOffset(fpi, nextSector, srcFileSize);
  }

  *startByte = start;
  *endByte = end;
  return true;
}

// Binary search .idx.fpi's per-group cumulative-ordinal field to find the group whose
// head fence word's ordinal is the largest value <= targetOrdinal, and resolve that
// fence word's byte offset in the source file. Unlike binarySearchFpi, this never reads
// a full sidecar sector or decodes any front-coded prefix: every probe is a direct
// single-field seek+read, since the ordinal field is self-contained per group. The
// caller (wordAtOrdinal) then linearly scans src forward from (*fenceByteOffset,
// *fenceOrdinal), counting entries, until it reaches targetOrdinal.
// Returns false if fpi is invalid/empty or targetOrdinal is out of range.
bool Dictionary::binarySearchFpiOrdinal(HalFile& fpi, uint32_t srcFileSize, uint32_t targetOrdinal,
                                        uint32_t* fenceByteOffset, uint32_t* fenceOrdinal) {
  const uint32_t sectorCount = srcFileSize == 0 ? 0 : fpiCeilDiv(srcFileSize, FPI_SECTOR_SIZE);
  if (sectorCount == 0) return false;
  const uint32_t groupCount = fpiCeilDiv(sectorCount, FPI_GROUP_SECTORS);
  if (groupCount == 0) return false;

  uint8_t versionByte = 0xff;
  fpi.seekSet(0);
  if (fpi.read(&versionByte, 1) != 1 || versionByte != FPI_VERSION) return false;

  // Find the last group whose head ordinal is <= targetOrdinal.
  uint32_t lo = 0, hi = groupCount - 1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo + 1) / 2;
    if (fpiReadGroupOrdinal(fpi, mid) <= targetOrdinal) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  const uint32_t ord = fpiReadGroupOrdinal(fpi, lo);
  if (ord == UINT32_MAX || ord > targetOrdinal) return false;

  // Reconstruct the group head sector's fence byte offset from its rel byte + high bit
  // (group[0] and bit 0 of group[8] — see generateFpi's assignFence).
  uint8_t relLow = 0, mask = 0;
  const uint32_t offset = fpiGroupFileOffset(lo);
  fpi.seekSet(offset + 0);
  if (fpi.read(&relLow, 1) != 1) return false;
  fpi.seekSet(offset + 8);
  if (fpi.read(&mask, 1) != 1) return false;

  uint32_t rel = relLow;
  if (mask & 0x01) rel |= 0x100;
  const uint32_t sector = lo * FPI_GROUP_SECTORS;
  *fenceByteOffset = sector * FPI_SECTOR_SIZE + rel;
  *fenceOrdinal = ord;
  return true;
}

// ---------------------------------------------------------------------------
// Reading helpers
// ---------------------------------------------------------------------------

std::string Dictionary::readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size) {
  HalFile dict;
  if (!Storage.openFileForRead("DICT", DictPaths(folderPath).dict().c_str(), dict)) return "";

  static constexpr uint32_t MAX_DEFINITION_BYTES = 256 * 1024;
  const uint32_t dictSize = static_cast<uint32_t>(dict.fileSize());
  if (offset > dictSize || size > dictSize - offset || size > MAX_DEFINITION_BYTES) {
    dict.close();
    return "";
  }

  dict.seekSet(offset);

  std::string def(size, '\0');
  int bytesRead = dict.read(reinterpret_cast<uint8_t*>(&def[0]), size);
  dict.close();

  if (bytesRead < 0) return "";
  if (static_cast<uint32_t>(bytesRead) < size) def.resize(bytesRead);
  return def;
}

// ---------------------------------------------------------------------------
// Locate (index search only — no definition read, zero RAM growth)
// ---------------------------------------------------------------------------

DictLocation Dictionary::locate(const std::string& word, const DictLookupCallbacks& cbs, const char* cachePath) {
  DictLocation result;
  result.folderPath = readDictPath(cachePath);
  if (result.folderPath.empty()) return result;

  DictPaths dp(result.folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return result;

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = idxFileSize;

  HalFile idxFpi;
  if (Storage.openFileForRead("DICT", dp.idxFpi().c_str(), idxFpi)) {
    if (!binarySearchFpi(idxFpi, word.c_str(), idxFileSize, &startByte, &endByte)) {
      startByte = 0;
      endByte = idxFileSize;
    }
    idxFpi.close();
  }

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 70);

  idx.seekSet(startByte);

  while (static_cast<uint32_t>(idx.position()) < endByte) {
    if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) {
      idx.close();
      return result;
    }

    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t suffix[8];
    if (idx.read(suffix, 8) != 8) break;

    int cmp = StringUtils::asciiCaseCmp(wordBuf, word.c_str());
    if (cmp == 0) {
      result.offset = fpiReadBe32(suffix);
      result.size = fpiReadBe32(suffix + 4);
      result.found = true;
      idx.close();
      if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
      return result;
    }

    if (cmp > 0) break;
  }

  idx.close();
  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
  return result;
}

// ---------------------------------------------------------------------------
// Lookup (convenience wrapper — locate + read into string)
// ---------------------------------------------------------------------------

std::string Dictionary::lookup(const std::string& word, const DictLookupCallbacks& cbs, const char* cachePath) {
  auto loc = locate(word, cbs, cachePath);
  if (!loc.found) return "";
  return readDefinition(loc.folderPath, loc.offset, loc.size);
}

// ---------------------------------------------------------------------------
// Alternate-form lookup (.syn)
// ---------------------------------------------------------------------------

// Resolve the word at 0-based ordinal in .idx using .idx.fpi's per-group cumulative
// ordinal field for a fast approximate seek, then a bounded linear scan to the exact
// ordinal. Falls back to a full linear scan from byte 0 if .idx.fpi is absent/invalid.
std::string Dictionary::wordAtOrdinal(const std::string& folderPath, uint32_t ordinal) {
  DictPaths dp(folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return "";

  uint32_t startByte = 0;
  uint32_t startOrdinal = 0;

  HalFile idxFpi;
  if (Storage.openFileForRead("DICT", dp.idxFpi().c_str(), idxFpi)) {
    const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
    if (!binarySearchFpiOrdinal(idxFpi, idxFileSize, ordinal, &startByte, &startOrdinal)) {
      startByte = 0;
      startOrdinal = 0;
    }
    idxFpi.close();
  }

  idx.seekSet(startByte);

  // Skip forward from startOrdinal to reach the target.
  for (uint32_t i = startOrdinal; i < ordinal; i++) {
    if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) {
      idx.close();
      return "";
    }
    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) {
      idx.close();
      return "";
    }
  }

  int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
  idx.close();
  if (len < 0) return "";
  return std::string(wordBuf, static_cast<size_t>(len));
}

std::string Dictionary::resolveAltForm(const std::string& word, const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return "";

  DictPaths dp(folderPath);
  if (!Storage.exists(dp.syn().c_str())) return "";

  HalFile syn;
  if (!Storage.openFileForRead("DICT", dp.syn().c_str(), syn)) return "";

  const uint32_t synFileSize = static_cast<uint32_t>(syn.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = synFileSize;

  HalFile synFpi;
  if (Storage.openFileForRead("DICT", dp.synFpi().c_str(), synFpi)) {
    if (!binarySearchFpi(synFpi, word.c_str(), synFileSize, &startByte, &endByte)) {
      startByte = 0;
      endByte = synFileSize;
    }
    synFpi.close();
  }

  syn.seekSet(startByte);

  while (static_cast<uint32_t>(syn.position()) < endByte) {
    int len = readWordInto(syn, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t idxBuf[4];
    if (syn.read(idxBuf, 4) != 4) break;

    int cmp = StringUtils::asciiCaseCmp(wordBuf, word.c_str());
    if (cmp == 0) {
      // Big-endian original word index in .idx
      uint32_t originalIdx = fpiReadBe32(idxBuf);
      syn.close();
      return wordAtOrdinal(folderPath, originalIdx);
    }

    if (cmp > 0) break;
  }

  syn.close();
  return "";
}

// ---------------------------------------------------------------------------
// Stemming
// ---------------------------------------------------------------------------

std::vector<std::string> Dictionary::getStemVariants(const std::string& word) {
  std::vector<std::string> variants;
  variants.reserve(8);
  size_t len = word.size();
  if (len < 3) return variants;

  auto endsWith = [&word, len](const char* suffix) {
    size_t slen = strlen(suffix);
    return len >= slen && word.compare(len - slen, slen, suffix) == 0;
  };

  // Build a variant into a reusable scratch buffer (keep the first `keep` chars
  // of `word`, then append `suffix`) and push it only if it is at least 2 chars
  // and not already present. Reusing `scratch` avoids the substr/concat
  // temporaries the previous implementation allocated per branch, and the inline
  // dedup (variants is bounded by reserve(8)) removes the O(n²) post-pass.
  std::string scratch;
  scratch.reserve(len + 4);
  auto addVariant = [&variants, &scratch, &word](size_t keep, const char* suffix) {
    scratch.assign(word, 0, keep);
    scratch.append(suffix);
    if (scratch.size() < 2) return;
    if (std::find(variants.begin(), variants.end(), scratch) != variants.end()) return;
    variants.push_back(scratch);
  };
  // Variant formed by dropping a leading prefix of `start` chars (prefix removal).
  auto addTail = [&variants, &scratch, &word, len](size_t start) {
    scratch.assign(word, start, len - start);
    if (scratch.size() < 2) return;
    if (std::find(variants.begin(), variants.end(), scratch) != variants.end()) return;
    variants.push_back(scratch);
  };

  // Plurals
  if (endsWith("sses")) addVariant(len - 2, "");
  if (endsWith("ses")) addVariant(len - 2, "is");
  if (endsWith("ies")) {
    addVariant(len - 3, "y");
    addVariant(len - 2, "");
  }
  if (endsWith("ves")) {
    addVariant(len - 3, "f");
    addVariant(len - 3, "fe");
    addVariant(len - 1, "");
  }
  if (endsWith("men")) addVariant(len - 3, "man");
  if (endsWith("es") && !endsWith("sses") && !endsWith("ies") && !endsWith("ves")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
  }
  if (endsWith("s") && !endsWith("ss") && !endsWith("us") && !endsWith("es")) {
    addVariant(len - 1, "");
  }

  // Past tense
  if (endsWith("ied")) {
    addVariant(len - 3, "y");
    addVariant(len - 1, "");
  }
  if (endsWith("ed") && !endsWith("ied")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
    if (len > 4 && word[len - 3] == word[len - 4]) {
      addVariant(len - 3, "");
    }
  }

  // Progressive
  if (endsWith("ying")) {
    addVariant(len - 4, "ie");
  }
  if (endsWith("ing") && !endsWith("ying")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
    if (len > 5 && word[len - 4] == word[len - 5]) {
      addVariant(len - 4, "");
    }
  }

  // Adverb
  if (endsWith("ically")) {
    addVariant(len - 6, "ic");
    addVariant(len - 4, "");
  }
  if (endsWith("ally") && !endsWith("ically")) {
    addVariant(len - 4, "al");
    addVariant(len - 2, "");
  }
  if (endsWith("ily") && !endsWith("ally")) {
    addVariant(len - 3, "y");
  }
  if (endsWith("ly") && !endsWith("ily") && !endsWith("ally")) {
    addVariant(len - 2, "");
  }

  // Comparative / superlative
  if (endsWith("ier")) {
    addVariant(len - 3, "y");
  }
  if (endsWith("er") && !endsWith("ier")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
    if (len > 4 && word[len - 3] == word[len - 4]) {
      addVariant(len - 3, "");
    }
  }
  if (endsWith("iest")) {
    addVariant(len - 4, "y");
  }
  if (endsWith("est") && !endsWith("iest")) {
    addVariant(len - 3, "");
    addVariant(len - 2, "");
    if (len > 5 && word[len - 4] == word[len - 5]) {
      addVariant(len - 4, "");
    }
  }

  // Derivational suffixes
  if (endsWith("ness")) addVariant(len - 4, "");
  if (endsWith("ment")) addVariant(len - 4, "");
  if (endsWith("ful")) addVariant(len - 3, "");
  if (endsWith("less")) addVariant(len - 4, "");
  if (endsWith("able")) {
    addVariant(len - 4, "");
    addVariant(len - 4, "e");
  }
  if (endsWith("ible")) {
    addVariant(len - 4, "");
    addVariant(len - 4, "e");
  }
  if (endsWith("ation")) {
    addVariant(len - 5, "");
    addVariant(len - 5, "e");
    addVariant(len - 5, "ate");
  }
  if (endsWith("tion") && !endsWith("ation")) {
    addVariant(len - 4, "te");
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ion") && !endsWith("tion")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("al") && !endsWith("ial")) {
    addVariant(len - 2, "");
    addVariant(len - 2, "e");
  }
  if (endsWith("ial")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ous")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ive")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ize")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ise")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("en")) {
    addVariant(len - 2, "");
    addVariant(len - 2, "e");
  }

  // Prefix removal
  if (len > 5 && word.compare(0, 2, "un") == 0) addTail(2);
  if (len > 6 && word.compare(0, 3, "dis") == 0) addTail(3);
  if (len > 6 && word.compare(0, 3, "mis") == 0) addTail(3);
  if (len > 6 && word.compare(0, 3, "pre") == 0) addTail(3);
  if (len > 7 && word.compare(0, 4, "over") == 0) addTail(4);
  if (len > 5 && word.compare(0, 2, "re") == 0) addTail(2);

  return variants;
}

// ---------------------------------------------------------------------------
// Fuzzy search (zero persistent RAM — uses findPageBounds for neighbourhood)
// ---------------------------------------------------------------------------

int Dictionary::editDistance(const std::string& a, const std::string& b, int maxDist) {
  int m = static_cast<int>(a.size());
  int n = static_cast<int>(b.size());
  if (std::abs(m - n) > maxDist) return maxDist + 1;

  std::vector<int> dp(n + 1);
  for (int j = 0; j <= n; j++) dp[j] = j;

  for (int i = 1; i <= m; i++) {
    int prev = dp[0];
    dp[0] = i;
    int rowMin = dp[0];
    for (int j = 1; j <= n; j++) {
      int temp = dp[j];
      if (a[i - 1] == b[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
      if (dp[j] < rowMin) rowMin = dp[j];
    }
    if (rowMin > maxDist) return maxDist + 1;
  }
  return dp[n];
}

std::vector<std::string> Dictionary::findSimilar(const std::string& word, int maxResults, const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return {};

  DictPaths dp(folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return {};

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t scanStart = 0;
  uint32_t scanEnd = idxFileSize;

  HalFile idxFpi;
  if (Storage.openFileForRead("DICT", dp.idxFpi().c_str(), idxFpi)) {
    if (!binarySearchFpi(idxFpi, word.c_str(), idxFileSize, &scanStart, &scanEnd, 7)) {
      scanStart = 0;
      scanEnd = idxFileSize;
    }
    idxFpi.close();
  }

  idx.seekSet(scanStart);

  int maxDist = std::max(2, static_cast<int>(word.size()) / 3 + 1);

  // Collect candidate words into one pooled buffer and sort lightweight
  // {offset, distance} records — avoids a heap std::string per candidate.
  std::string pool;
  pool.reserve(256);
  struct Candidate {
    uint16_t offset;
    uint16_t distance;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(maxResults) * 4);

  while (static_cast<uint32_t>(idx.position()) < scanEnd) {
    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) break;

    if (len == 0) continue;
    if (StringUtils::asciiCaseCmp(wordBuf, word.c_str()) == 0) continue;

    int dist = editDistance(wordBuf, word, maxDist);
    if (dist <= maxDist) {
      // Stop pooling if the offset would overflow uint16_t (pathological window).
      if (pool.size() + static_cast<size_t>(len) + 1 > 0xFFFF) break;
      const uint16_t offset = TextPool::append(pool, wordBuf, static_cast<size_t>(len));
      candidates.push_back({offset, static_cast<uint16_t>(dist)});
    }
  }

  idx.close();

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

  std::vector<std::string> results;
  results.reserve(static_cast<size_t>(maxResults));
  for (size_t i = 0; i < candidates.size() && static_cast<int>(results.size()) < maxResults; i++) {
    results.emplace_back(pool.data() + candidates[i].offset);
  }
  return results;
}
