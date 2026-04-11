#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

// Static member definitions
char Dictionary::wordBuf[256] = "";

namespace {
constexpr char DICT_BIN[] = "dictionary.bin";
constexpr char GLOBAL_DICT_DIR[] = "/.crosspoint";
}  // namespace

// OFT file constants (StarDict Cache format, verified against real files).
// Header: 30-byte text + 8-byte fixed magic = 38 bytes total.
// Each entry: LE uint32 = byte offset of word (K+1)*32 in the source file.
static constexpr uint32_t OFT_HEADER_SIZE = 38;
static constexpr uint32_t OFT_STRIDE = 32;  // words per page

// .idx.oft.cspt file constants (CrossPoint optimized index).
// Header: 12 bytes = magic(4) + version(1) + prefixLen(1) + stride(2) + entryCount(4).
// Each entry: prefixLen bytes (null-padded headword) + 4-byte LE idx offset = 20 bytes.
static constexpr uint8_t CSPT_MAGIC[4] = {'C', 'S', 'P', 'T'};
static constexpr uint8_t CSPT_VERSION = 1;
static constexpr uint8_t CSPT_PREFIX_LEN = 16;
static constexpr uint16_t CSPT_STRIDE = 16;
static constexpr uint32_t CSPT_HEADER_SIZE = 12;
static constexpr uint32_t CSPT_ENTRY_SIZE = CSPT_PREFIX_LEN + 4;  // 20 bytes

// ---------------------------------------------------------------------------
// Path management
// ---------------------------------------------------------------------------

std::string Dictionary::readDictPath(const char* cachePath) {
  char binPath[128];

  // Try per-book dictionary.bin first when cachePath is provided.
  if (cachePath && cachePath[0] != '\0') {
    snprintf(binPath, sizeof(binPath), "%s/%s", cachePath, DICT_BIN);
    FsFile f;
    if (Storage.openFileForRead("DICT", binPath, f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
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
  FsFile f;
  if (!Storage.openFileForRead("DICT", binPath, f)) return "";
  const int sz = static_cast<int>(f.fileSize());
  if (sz <= 0) {
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
  FsFile f;
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

bool Dictionary::hasAltForms(const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return false;
  return Storage.exists(DictPaths(folderPath).syn().c_str());
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

  FsFile file;
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
        } else if (strcmp(keyBuf, "synwordcount") == 0) {
          isNumField = true;
          valNum = &info.altFormCount;
          info.hasAltForms = true;
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

int Dictionary::readWordInto(FsFile& file, char* buf, size_t bufSize) {
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
// OFT binary search helper
// ---------------------------------------------------------------------------

// Case-insensitive strcmp for ASCII — used in findPageBounds() because StarDict
// dictionaries (including wiktionary-derived ones) are sorted case-insensitively.
// Using plain strcmp would cause the binary search to land on the wrong page for
// any word whose alphabetic neighbourhood contains mixed-case page boundaries.
static int cistrcmp(const char* a, const char* b) {
  while (*a && *b) {
    int diff = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    a++;
    b++;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

void Dictionary::findPageBounds(FsFile& oft, FsFile& src, uint32_t srcFileSize, const char* target, uint32_t* startByte,
                                uint32_t* endByte) {
  const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
  const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;
  const uint32_t numPages = numEntries + 1;  // page 0 is implicit (starts at byte 0 in src)

  if (numEntries == 0) {
    // No OFT entries — entire source file is one page
    *startByte = 0;
    *endByte = srcFileSize;
    return;
  }

  // Returns the byte offset in src where page K begins.
  // Page 0 always starts at 0; page K>0 is stored in OFT entry K-1 (LE uint32).
  auto pageStart = [&](uint32_t k) -> uint32_t {
    if (k == 0) return 0;
    oft.seekSet(OFT_HEADER_SIZE + (k - 1) * 4);
    uint8_t raw[4];
    if (oft.read(raw, 4) != 4) return srcFileSize;
    uint32_t val;
    memcpy(&val, raw, 4);  // little-endian on ESP32-C3
    return val;
  };

  // Binary search: find the last page whose first word <= target
  uint32_t lo = 0, hi = numPages - 1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    uint32_t midStart = pageStart(mid);
    src.seekSet(midStart);
    int len = readWordInto(src, wordBuf, sizeof(wordBuf));
    if (len < 0 || cistrcmp(wordBuf, target) > 0) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  *startByte = pageStart(lo);
  *endByte = (lo + 1 < numPages) ? pageStart(lo + 1) : srcFileSize;
}

// ---------------------------------------------------------------------------
// Reading helpers
// ---------------------------------------------------------------------------

std::string Dictionary::readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size) {
  FsFile dict;
  if (!Storage.openFileForRead("DICT", DictPaths(folderPath).dict().c_str(), dict)) return "";

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
  FsFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return result;

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = idxFileSize;

  FsFile oft;
  if (Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft)) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &startByte, &endByte);
    oft.close();
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

    int cmp = cistrcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      result.offset = (static_cast<uint32_t>(suffix[0]) << 24) | (static_cast<uint32_t>(suffix[1]) << 16) |
                      (static_cast<uint32_t>(suffix[2]) << 8) | static_cast<uint32_t>(suffix[3]);
      result.size = (static_cast<uint32_t>(suffix[4]) << 24) | (static_cast<uint32_t>(suffix[5]) << 16) |
                    (static_cast<uint32_t>(suffix[6]) << 8) | static_cast<uint32_t>(suffix[7]);
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

// Resolve the word at 0-based ordinal in .idx using .idx.oft for fast page seek.
std::string Dictionary::wordAtOrdinal(const std::string& folderPath, uint32_t ordinal) {
  DictPaths dp(folderPath);
  FsFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return "";

  const uint32_t pageNum = ordinal / OFT_STRIDE;
  const uint32_t withinPage = ordinal % OFT_STRIDE;
  uint32_t pageStartByte = 0;

  if (pageNum > 0) {
    FsFile oft;
    if (Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft)) {
      oft.seekSet(OFT_HEADER_SIZE + (pageNum - 1) * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) == 4) memcpy(&pageStartByte, raw, 4);  // LE uint32
      oft.close();
    }
  }

  idx.seekSet(pageStartByte);

  // Skip `withinPage` entries to reach the target
  for (uint32_t i = 0; i < withinPage; i++) {
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

  FsFile syn;
  if (!Storage.openFileForRead("DICT", dp.syn().c_str(), syn)) return "";

  const uint32_t synFileSize = static_cast<uint32_t>(syn.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = synFileSize;

  FsFile oft;
  if (Storage.openFileForRead("DICT", dp.synOft().c_str(), oft)) {
    findPageBounds(oft, syn, synFileSize, word.c_str(), &startByte, &endByte);
    oft.close();
  }

  syn.seekSet(startByte);

  while (static_cast<uint32_t>(syn.position()) < endByte) {
    int len = readWordInto(syn, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t idxBuf[4];
    if (syn.read(idxBuf, 4) != 4) break;

    int cmp = cistrcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      // Big-endian original word index in .idx
      uint32_t originalIdx = (static_cast<uint32_t>(idxBuf[0]) << 24) | (static_cast<uint32_t>(idxBuf[1]) << 16) |
                             (static_cast<uint32_t>(idxBuf[2]) << 8) | static_cast<uint32_t>(idxBuf[3]);
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

  auto add = [&variants](const std::string& s) {
    if (s.size() >= 2) variants.push_back(s);
  };

  // Plurals
  if (endsWith("sses")) add(word.substr(0, len - 2));
  if (endsWith("ses")) add(word.substr(0, len - 2) + "is");
  if (endsWith("ies")) {
    add(word.substr(0, len - 3) + "y");
    add(word.substr(0, len - 2));
  }
  if (endsWith("ves")) {
    add(word.substr(0, len - 3) + "f");
    add(word.substr(0, len - 3) + "fe");
    add(word.substr(0, len - 1));
  }
  if (endsWith("men")) add(word.substr(0, len - 3) + "man");
  if (endsWith("es") && !endsWith("sses") && !endsWith("ies") && !endsWith("ves")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
  }
  if (endsWith("s") && !endsWith("ss") && !endsWith("us") && !endsWith("es")) {
    add(word.substr(0, len - 1));
  }

  // Past tense
  if (endsWith("ied")) {
    add(word.substr(0, len - 3) + "y");
    add(word.substr(0, len - 1));
  }
  if (endsWith("ed") && !endsWith("ied")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
    if (len > 4 && word[len - 3] == word[len - 4]) {
      add(word.substr(0, len - 3));
    }
  }

  // Progressive
  if (endsWith("ying")) {
    add(word.substr(0, len - 4) + "ie");
  }
  if (endsWith("ing") && !endsWith("ying")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
    if (len > 5 && word[len - 4] == word[len - 5]) {
      add(word.substr(0, len - 4));
    }
  }

  // Adverb
  if (endsWith("ically")) {
    add(word.substr(0, len - 6) + "ic");
    add(word.substr(0, len - 4));
  }
  if (endsWith("ally") && !endsWith("ically")) {
    add(word.substr(0, len - 4) + "al");
    add(word.substr(0, len - 2));
  }
  if (endsWith("ily") && !endsWith("ally")) {
    add(word.substr(0, len - 3) + "y");
  }
  if (endsWith("ly") && !endsWith("ily") && !endsWith("ally")) {
    add(word.substr(0, len - 2));
  }

  // Comparative / superlative
  if (endsWith("ier")) {
    add(word.substr(0, len - 3) + "y");
  }
  if (endsWith("er") && !endsWith("ier")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
    if (len > 4 && word[len - 3] == word[len - 4]) {
      add(word.substr(0, len - 3));
    }
  }
  if (endsWith("iest")) {
    add(word.substr(0, len - 4) + "y");
  }
  if (endsWith("est") && !endsWith("iest")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 2));
    if (len > 5 && word[len - 4] == word[len - 5]) {
      add(word.substr(0, len - 4));
    }
  }

  // Derivational suffixes
  if (endsWith("ness")) add(word.substr(0, len - 4));
  if (endsWith("ment")) add(word.substr(0, len - 4));
  if (endsWith("ful")) add(word.substr(0, len - 3));
  if (endsWith("less")) add(word.substr(0, len - 4));
  if (endsWith("able")) {
    add(word.substr(0, len - 4));
    add(word.substr(0, len - 4) + "e");
  }
  if (endsWith("ible")) {
    add(word.substr(0, len - 4));
    add(word.substr(0, len - 4) + "e");
  }
  if (endsWith("ation")) {
    add(word.substr(0, len - 5));
    add(word.substr(0, len - 5) + "e");
    add(word.substr(0, len - 5) + "ate");
  }
  if (endsWith("tion") && !endsWith("ation")) {
    add(word.substr(0, len - 4) + "te");
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ion") && !endsWith("tion")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("al") && !endsWith("ial")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 2) + "e");
  }
  if (endsWith("ial")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ous")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ive")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ize")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ise")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("en")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 2) + "e");
  }

  // Prefix removal
  if (len > 5 && word.compare(0, 2, "un") == 0) add(word.substr(2));
  if (len > 6 && word.compare(0, 3, "dis") == 0) add(word.substr(3));
  if (len > 6 && word.compare(0, 3, "mis") == 0) add(word.substr(3));
  if (len > 6 && word.compare(0, 3, "pre") == 0) add(word.substr(3));
  if (len > 7 && word.compare(0, 4, "over") == 0) add(word.substr(4));
  if (len > 5 && word.compare(0, 2, "re") == 0) add(word.substr(2));

  // Deduplicate preserving insertion order
  std::vector<std::string> deduped;
  deduped.reserve(variants.size());
  std::copy_if(variants.begin(), variants.end(), std::back_inserter(deduped), [&deduped](const std::string& v) {
    return std::find(deduped.begin(), deduped.end(), v) == deduped.end();
  });
  return deduped;
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
  FsFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return {};

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t centerStart = 0;
  uint32_t centerEnd = idxFileSize;

  FsFile oft;
  const bool hasOft = Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft);

  if (hasOft) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &centerStart, &centerEnd);
  }

  // Extend the scan window by ±7 pages around the found neighbourhood page.
  // Each page is approximately (centerEnd - centerStart) bytes.
  const uint32_t pageSize = (centerEnd > centerStart) ? (centerEnd - centerStart) : 1;
  static constexpr uint32_t PAGE_RADIUS = 7;
  const uint32_t scanStart = (centerStart > PAGE_RADIUS * pageSize) ? (centerStart - PAGE_RADIUS * pageSize) : 0;
  const uint32_t scanEnd = std::min(idxFileSize, centerEnd + PAGE_RADIUS * pageSize);

  if (hasOft) {
    // Snap scanStart back to the true page boundary containing it via binary search.
    // Re-use findPageBounds with the first word of the scan region as target would be complex;
    // instead just clamp to a page-aligned position by seeking to the OFT entry.
    // For simplicity, snap to the nearest OFT page boundary at or before scanStart.
    const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
    const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;

    // Walk backward through OFT entries to find the largest entry <= scanStart
    uint32_t snappedStart = 0;
    for (uint32_t i = 0; i < numEntries; i++) {
      oft.seekSet(OFT_HEADER_SIZE + i * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) != 4) break;
      uint32_t entryVal;
      memcpy(&entryVal, raw, 4);
      if (entryVal <= scanStart) {
        snappedStart = entryVal;
      } else {
        break;  // OFT entries are monotonically increasing
      }
    }
    oft.close();

    idx.seekSet(snappedStart);
  } else {
    idx.seekSet(scanStart);
  }

  int maxDist = std::max(2, static_cast<int>(word.size()) / 3 + 1);

  struct Candidate {
    std::string text;
    int distance;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(maxResults) * 4);

  while (static_cast<uint32_t>(idx.position()) < scanEnd) {
    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) break;

    if (len == 0) continue;
    if (cistrcmp(wordBuf, word.c_str()) == 0) continue;

    int dist = editDistance(wordBuf, word, maxDist);
    if (dist <= maxDist) {
      candidates.push_back({std::string(wordBuf, static_cast<size_t>(len)), dist});
    }
  }

  idx.close();

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

  std::vector<std::string> results;
  results.reserve(static_cast<size_t>(maxResults));
  for (size_t i = 0; i < candidates.size() && static_cast<int>(results.size()) < maxResults; i++) {
    results.push_back(candidates[i].text);
  }
  return results;
}
