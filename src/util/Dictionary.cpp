#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>

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

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 70);
  idx.seekSet(0);

  while (idx.available()) {
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
// Fuzzy search
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
  idx.seekSet(0);

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

  while (idx.available()) {
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
