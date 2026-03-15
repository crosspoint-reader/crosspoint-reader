#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// Static member definition
char Dictionary::activeFolderPath[500] = "";

// OFT file constants (StarDict Cache format, verified against real files).
// Header: 30-byte text + 8-byte fixed magic = 38 bytes total.
// Each entry: LE uint32 = byte offset of word (K+1)*32 in the source file.
static constexpr uint32_t OFT_HEADER_SIZE = 38;
static constexpr uint32_t OFT_STRIDE      = 32;  // words per page

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

void Dictionary::buildPath(char* buf, size_t len, const char* ext) {
  // activeFolderPath is the full base path (e.g. /dictionary/dict-en-en/dict-data).
  // Callers pass just the extension (e.g. "idx"), giving /dictionary/.../dict-data.idx.
  snprintf(buf, len, "%s.%s", activeFolderPath, ext);
}

// ---------------------------------------------------------------------------
// Active path management
// ---------------------------------------------------------------------------

void Dictionary::setActivePath(const char* folderPath) {
  if (folderPath == nullptr || folderPath[0] == '\0') {
    activeFolderPath[0] = '\0';
  } else {
    strncpy(activeFolderPath, folderPath, sizeof(activeFolderPath) - 1);
    activeFolderPath[sizeof(activeFolderPath) - 1] = '\0';
  }
}

const char* Dictionary::getActivePath() { return activeFolderPath; }

// ---------------------------------------------------------------------------
// Validity checks
// ---------------------------------------------------------------------------

bool Dictionary::exists() {
  if (activeFolderPath[0] == '\0') return false;
  char idxPath[520];
  char dictPath[520];
  buildPath(idxPath, sizeof(idxPath), "idx");
  buildPath(dictPath, sizeof(dictPath), "dict");
  return Storage.exists(idxPath) && Storage.exists(dictPath);
}

bool Dictionary::hasSyn() {
  if (activeFolderPath[0] == '\0') return false;
  char synPath[520];
  buildPath(synPath, sizeof(synPath), "syn");
  return Storage.exists(synPath);
}

bool Dictionary::isValidDictionary() {
  const char* folderPath = SETTINGS.dictionaryPath;
  if (folderPath[0] == '\0') return false;
  // Single buffer reused for each path check — 3 x char[520] simultaneously would waste 1040 bytes.
  char pathBuf[520];
  snprintf(pathBuf, sizeof(pathBuf), "%s.ifo", folderPath);
  const bool ifoExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.idx", folderPath);
  const bool idxExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.dict", folderPath);
  const bool valid = ifoExists && idxExists && Storage.exists(pathBuf);
  if (!valid) {
    LOG_DBG("DICT", "Stored dictionary path no longer valid, resetting");
    SETTINGS.dictionaryPath[0] = '\0';
    setActivePath("");
    SETTINGS.saveToFile();
  }
  return valid;
}

// ---------------------------------------------------------------------------
// .ifo parsing
// ---------------------------------------------------------------------------

DictInfo Dictionary::readInfo(const char* folderPath) {
  DictInfo info;
  if (folderPath == nullptr || folderPath[0] == '\0') return info;

  // Single path buffer reused for all three path checks — saves 1040 bytes vs 3 x char[520].
  char pathBuf[520];
  snprintf(pathBuf, sizeof(pathBuf), "%s.ifo", folderPath);

  FsFile file;
  if (!Storage.openFileForRead("DICT", pathBuf, file)) return info;

  // Read entire .ifo into a stack buffer (these files are tiny, < 512 bytes)
  char buf[512];
  int bytesRead = file.read(buf, sizeof(buf) - 1);
  file.close();
  if (bytesRead <= 0) return info;
  buf[bytesRead] = '\0';

  // Validate header line
  static const char* HEADER = "StarDict's dict ifo file";
  if (strncmp(buf, HEADER, strlen(HEADER)) != 0) {
    LOG_ERR("DICT", "Invalid .ifo header in %s", folderPath);
    return info;
  }

  // Parse key=value lines
  char* line = buf;
  while ((line = strchr(line, '\n')) != nullptr) {
    line++;  // move past '\n'
    char* eq = strchr(line, '=');
    if (eq == nullptr) continue;

    *eq = '\0';
    const char* key = line;
    const char* val = eq + 1;

    // Strip trailing \r and \n from value, saving both so the outer loop's
    // strchr(line, '\n') can still find the next line's newline.
    char* cr = const_cast<char*>(strchr(val, '\r'));
    char savedCr = '\0';
    if (cr) { savedCr = *cr; *cr = '\0'; }
    char* nl = const_cast<char*>(strchr(val, '\n'));
    char savedNl = '\0';
    if (nl) { savedNl = *nl; *nl = '\0'; }

    if (strcmp(key, "bookname") == 0) {
      strncpy(info.bookname, val, sizeof(info.bookname) - 1);
    } else if (strcmp(key, "wordcount") == 0) {
      info.wordcount = static_cast<uint32_t>(atol(val));
    } else if (strcmp(key, "synwordcount") == 0) {
      info.synwordcount = static_cast<uint32_t>(atol(val));
      info.hasSyn = true;
    } else if (strcmp(key, "idxfilesize") == 0) {
      info.idxfilesize = static_cast<uint32_t>(atol(val));
    } else if (strcmp(key, "sametypesequence") == 0) {
      strncpy(info.sametypesequence, val, sizeof(info.sametypesequence) - 1);
    } else if (strcmp(key, "website") == 0) {
      strncpy(info.website, val, sizeof(info.website) - 1);
    } else if (strcmp(key, "date") == 0) {
      strncpy(info.date, val, sizeof(info.date) - 1);
    } else if (strcmp(key, "description") == 0) {
      strncpy(info.description, val, sizeof(info.description) - 1);
    }

    // Restore all modified characters so the outer loop can continue correctly.
    if (nl) *nl = savedNl;
    if (cr) *cr = savedCr;
    *eq = '=';  // restore for next iteration
  }

  // Check for compressed .dict.dz (but no .dict) — reuse pathBuf from above.
  snprintf(pathBuf, sizeof(pathBuf), "%s.dict", folderPath);
  const bool dictExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.dict.dz", folderPath);
  info.isCompressed = !dictExists && Storage.exists(pathBuf);

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
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
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
  do { ch = file.read(); } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

// ---------------------------------------------------------------------------
// OFT binary search helper
// ---------------------------------------------------------------------------

void Dictionary::findPageBounds(FsFile& oft, FsFile& src, uint32_t srcFileSize,
                                const char* target, uint32_t* startByte, uint32_t* endByte) {
  const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
  const uint32_t numEntries =
      (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;
  const uint32_t numPages = numEntries + 1;  // page 0 is implicit (starts at byte 0 in src)

  if (numEntries == 0) {
    // No OFT entries — entire source file is one page
    *startByte = 0;
    *endByte   = srcFileSize;
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

  char wordBuf[256];

  // Binary search: find the last page whose first word <= target
  uint32_t lo = 0, hi = numPages - 1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    uint32_t midStart = pageStart(mid);
    src.seekSet(midStart);
    int len = readWordInto(src, wordBuf, sizeof(wordBuf));
    if (len < 0 || strcmp(wordBuf, target) > 0) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  *startByte = pageStart(lo);
  *endByte   = (lo + 1 < numPages) ? pageStart(lo + 1) : srcFileSize;
}

// ---------------------------------------------------------------------------
// Reading helpers
// ---------------------------------------------------------------------------

std::string Dictionary::readDefinition(uint32_t offset, uint32_t size) {
  if (!exists()) return "";

  char dictPath[520];
  buildPath(dictPath, sizeof(dictPath), "dict");

  FsFile dict;
  if (!Storage.openFileForRead("DICT", dictPath, dict)) return "";

  dict.seekSet(offset);

  std::string def(size, '\0');
  int bytesRead = dict.read(reinterpret_cast<uint8_t*>(&def[0]), size);
  dict.close();

  if (bytesRead < 0) return "";
  if (static_cast<uint32_t>(bytesRead) < size) def.resize(bytesRead);
  return def;
}

// ---------------------------------------------------------------------------
// Lookup (zero persistent RAM — no static index)
// ---------------------------------------------------------------------------

std::string Dictionary::lookup(const std::string& word,
                               const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  if (!exists()) return "";

  // Single buffer reused for both path lookups — saves 520 bytes vs 2 x char[520].
  char pathBuf[520];
  buildPath(pathBuf, sizeof(pathBuf), "idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", pathBuf, idx)) return "";

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte   = idxFileSize;

  buildPath(pathBuf, sizeof(pathBuf), "idx.oft");
  FsFile oft;
  if (Storage.openFileForRead("DICT", pathBuf, oft)) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &startByte, &endByte);
    oft.close();
  }

  if (onProgress) onProgress(70);

  // Linear scan within the identified page (≤ OFT_STRIDE entries)
  idx.seekSet(startByte);
  char wordBuf[256];

  while (static_cast<uint32_t>(idx.position()) < endByte) {
    if (shouldCancel && shouldCancel()) {
      idx.close();
      return "";
    }

    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t suffix[8];
    if (idx.read(suffix, 8) != 8) break;

    int cmp = strcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      // Big-endian offset and size in .idx
      uint32_t dictOffset = (static_cast<uint32_t>(suffix[0]) << 24) |
                            (static_cast<uint32_t>(suffix[1]) << 16) |
                            (static_cast<uint32_t>(suffix[2]) << 8)  |
                            static_cast<uint32_t>(suffix[3]);
      uint32_t dictSize   = (static_cast<uint32_t>(suffix[4]) << 24) |
                            (static_cast<uint32_t>(suffix[5]) << 16) |
                            (static_cast<uint32_t>(suffix[6]) << 8)  |
                            static_cast<uint32_t>(suffix[7]);
      idx.close();
      if (onProgress) onProgress(100);
      return readDefinition(dictOffset, dictSize);
    }

    if (cmp > 0) break;  // Passed the target alphabetically — not found
  }

  idx.close();
  if (onProgress) onProgress(100);
  return "";
}

// ---------------------------------------------------------------------------
// Synonym lookup
// ---------------------------------------------------------------------------

// Resolve the word at 0-based ordinal in .idx using .idx.oft for fast page seek.
std::string Dictionary::wordAtOrdinal(uint32_t ordinal) {
  // Single buffer reused for both path lookups — saves 520 bytes vs 2 x char[520].
  char pathBuf[520];
  buildPath(pathBuf, sizeof(pathBuf), "idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", pathBuf, idx)) return "";

  const uint32_t pageNum     = ordinal / OFT_STRIDE;
  const uint32_t withinPage  = ordinal % OFT_STRIDE;
  uint32_t pageStartByte = 0;

  if (pageNum > 0) {
    buildPath(pathBuf, sizeof(pathBuf), "idx.oft");
    FsFile oft;
    if (Storage.openFileForRead("DICT", pathBuf, oft)) {
      oft.seekSet(OFT_HEADER_SIZE + (pageNum - 1) * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) == 4) memcpy(&pageStartByte, raw, 4);  // LE uint32
      oft.close();
    }
  }

  idx.seekSet(pageStartByte);

  char wordBuf[256];

  // Skip `withinPage` entries to reach the target
  for (uint32_t i = 0; i < withinPage; i++) {
    if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) { idx.close(); return ""; }
    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) { idx.close(); return ""; }
  }

  int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
  idx.close();
  if (len < 0) return "";
  return std::string(wordBuf, static_cast<size_t>(len));
}

std::string Dictionary::lookupSynonym(const std::string& word) {
  if (!hasSyn()) return "";

  // Single buffer reused for both path lookups — saves 520 bytes vs 2 x char[520].
  char pathBuf[520];
  buildPath(pathBuf, sizeof(pathBuf), "syn");

  FsFile syn;
  if (!Storage.openFileForRead("DICT", pathBuf, syn)) return "";

  const uint32_t synFileSize = static_cast<uint32_t>(syn.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte   = synFileSize;

  buildPath(pathBuf, sizeof(pathBuf), "syn.oft");
  FsFile oft;
  if (Storage.openFileForRead("DICT", pathBuf, oft)) {
    findPageBounds(oft, syn, synFileSize, word.c_str(), &startByte, &endByte);
    oft.close();
  }

  syn.seekSet(startByte);
  char wordBuf[256];

  while (static_cast<uint32_t>(syn.position()) < endByte) {
    int len = readWordInto(syn, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t idxBuf[4];
    if (syn.read(idxBuf, 4) != 4) break;

    int cmp = strcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      // Big-endian original word index in .idx
      uint32_t originalIdx = (static_cast<uint32_t>(idxBuf[0]) << 24) |
                             (static_cast<uint32_t>(idxBuf[1]) << 16) |
                             (static_cast<uint32_t>(idxBuf[2]) << 8)  |
                             static_cast<uint32_t>(idxBuf[3]);
      syn.close();
      return wordAtOrdinal(originalIdx);
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
  for (const auto& v : variants) {
    if (std::find(deduped.begin(), deduped.end(), v) != deduped.end()) continue;
    // cppcheck-suppress useStlAlgorithm
    deduped.push_back(v);
  }
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

std::vector<std::string> Dictionary::findSimilar(const std::string& word, int maxResults) {
  if (!exists()) return {};

  // Single buffer reused for both path lookups — saves 520 bytes vs 2 x char[520].
  char pathBuf[520];
  buildPath(pathBuf, sizeof(pathBuf), "idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", pathBuf, idx)) return {};

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t centerStart = 0;
  uint32_t centerEnd   = idxFileSize;

  buildPath(pathBuf, sizeof(pathBuf), "idx.oft");
  FsFile oft;
  const bool hasOft = Storage.openFileForRead("DICT", pathBuf, oft);

  if (hasOft) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &centerStart, &centerEnd);
  }

  // Extend the scan window by ±7 pages around the found neighbourhood page.
  // Each page is approximately (centerEnd - centerStart) bytes.
  const uint32_t pageSize = (centerEnd > centerStart) ? (centerEnd - centerStart) : 1;
  static constexpr uint32_t PAGE_RADIUS = 7;
  const uint32_t scanStart =
      (centerStart > PAGE_RADIUS * pageSize) ? (centerStart - PAGE_RADIUS * pageSize) : 0;
  const uint32_t scanEnd =
      std::min(idxFileSize, centerEnd + PAGE_RADIUS * pageSize);

  if (hasOft) {
    // Snap scanStart back to the true page boundary containing it via binary search.
    // Re-use findPageBounds with the first word of the scan region as target would be complex;
    // instead just clamp to a page-aligned position by seeking to the OFT entry.
    // For simplicity, snap to the nearest OFT page boundary at or before scanStart.
    const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
    const uint32_t numEntries =
        (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;

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

  char wordBuf[256];

  while (static_cast<uint32_t>(idx.position()) < scanEnd) {
    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) break;

    if (len == 0) continue;
    if (strcmp(wordBuf, word.c_str()) == 0) continue;

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
