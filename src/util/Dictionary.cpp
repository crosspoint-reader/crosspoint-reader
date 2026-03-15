#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// Static member definitions
char Dictionary::activeFolderPath[500] = "";
std::vector<uint32_t> Dictionary::sparseOffsets;
uint32_t Dictionary::totalWords = 0;
bool Dictionary::indexLoaded = false;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

void Dictionary::buildPath(char* buf, size_t len, const char* filename) {
  snprintf(buf, len, "%s/%s", activeFolderPath, filename);
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
  // Reset index so next lookup re-builds from new path.
  indexLoaded = false;
  sparseOffsets.clear();
  totalWords = 0;
}

const char* Dictionary::getActivePath() { return activeFolderPath; }

// ---------------------------------------------------------------------------
// Validity checks
// ---------------------------------------------------------------------------

bool Dictionary::exists() {
  if (activeFolderPath[0] == '\0') return false;
  char idxPath[520];
  char dictPath[520];
  buildPath(idxPath, sizeof(idxPath), "dict-data.idx");
  buildPath(dictPath, sizeof(dictPath), "dict-data.dict");
  return Storage.exists(idxPath) && Storage.exists(dictPath);
}

bool Dictionary::isValidDictionary() {
  const char* folderPath = SETTINGS.dictionaryPath;
  if (folderPath[0] == '\0') return false;
  char ifoPath[520];
  char idxPath[520];
  char dictPath[520];
  snprintf(ifoPath, sizeof(ifoPath), "%s/dict-data.ifo", folderPath);
  snprintf(idxPath, sizeof(idxPath), "%s/dict-data.idx", folderPath);
  snprintf(dictPath, sizeof(dictPath), "%s/dict-data.dict", folderPath);
  const bool valid = Storage.exists(ifoPath) && Storage.exists(idxPath) && Storage.exists(dictPath);
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

  char ifoPath[520];
  snprintf(ifoPath, sizeof(ifoPath), "%s/dict-data.ifo", folderPath);

  FsFile file;
  if (!Storage.openFileForRead("DICT", ifoPath, file)) return info;

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

  // Check for compressed .dict.dz (but no .dict)
  char dictPath[520];
  char dictDzPath[520];
  snprintf(dictPath, sizeof(dictPath), "%s/dict-data.dict", folderPath);
  snprintf(dictDzPath, sizeof(dictDzPath), "%s/dict-data.dict.dz", folderPath);
  info.isCompressed = !Storage.exists(dictPath) && Storage.exists(dictDzPath);

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
// Index loading
// ---------------------------------------------------------------------------

bool Dictionary::loadIndex(const std::function<void(int percent)>& onProgress,
                           const std::function<bool()>& shouldCancel) {
  char idxPath[520];
  buildPath(idxPath, sizeof(idxPath), "dict-data.idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", idxPath, idx)) return false;

  const uint32_t fileSize = static_cast<uint32_t>(idx.fileSize());

  sparseOffsets.clear();
  totalWords = 0;

  uint32_t pos = 0;
  int lastReportedPercent = -1;

  while (pos < fileSize) {
    if (shouldCancel && (totalWords % 100 == 0) && shouldCancel()) {
      idx.close();
      sparseOffsets.clear();
      totalWords = 0;
      return false;
    }

    if (totalWords % SPARSE_INTERVAL == 0) {
      sparseOffsets.push_back(pos);
    }

    // Skip word (null-terminated)
    int ch;
    do {
      ch = idx.read();
      if (ch < 0) {
        pos = fileSize;
        break;
      }
      pos++;
    } while (ch != 0);

    if (pos >= fileSize) break;

    // Skip 8 bytes (4-byte offset + 4-byte size)
    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) break;
    pos += 8;

    totalWords++;

    if (onProgress && fileSize > 0) {
      int percent = static_cast<int>(static_cast<uint64_t>(pos) * 90 / fileSize);
      if (percent > lastReportedPercent + 4) {
        lastReportedPercent = percent;
        onProgress(percent);
      }
    }
  }

  idx.close();
  indexLoaded = true;
  return totalWords > 0;
}

// ---------------------------------------------------------------------------
// Reading helpers
// ---------------------------------------------------------------------------

std::string Dictionary::readWord(FsFile& file) {
  std::string word;
  while (true) {
    int ch = file.read();
    if (ch <= 0) break;
    word += static_cast<char>(ch);
  }
  return word;
}

std::string Dictionary::readDefinition(uint32_t offset, uint32_t size) {
  if (!exists()) return "";

  char dictPath[520];
  buildPath(dictPath, sizeof(dictPath), "dict-data.dict");

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
// Lookup
// ---------------------------------------------------------------------------

std::string Dictionary::lookup(const std::string& word,
                               const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  if (!exists()) return "";

  if (!indexLoaded) {
    if (!loadIndex(onProgress, shouldCancel)) return "";
  }

  if (sparseOffsets.empty()) return "";

  char idxPath[520];
  buildPath(idxPath, sizeof(idxPath), "dict-data.idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", idxPath, idx)) return "";

  // Binary search the sparse offset table
  int lo = 0, hi = static_cast<int>(sparseOffsets.size()) - 1;

  while (lo < hi) {
    if (shouldCancel && shouldCancel()) {
      idx.close();
      return "";
    }

    int mid = lo + (hi - lo + 1) / 2;
    idx.seekSet(sparseOffsets[mid]);
    std::string key = readWord(idx);

    if (key <= word) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  if (onProgress) onProgress(95);

  // Linear scan within segment
  idx.seekSet(sparseOffsets[lo]);

  int maxEntries = SPARSE_INTERVAL;
  if (lo == static_cast<int>(sparseOffsets.size()) - 1) {
    maxEntries = static_cast<int>(totalWords - static_cast<uint32_t>(lo) * SPARSE_INTERVAL);
  }

  for (int i = 0; i < maxEntries; i++) {
    if (shouldCancel && shouldCancel()) {
      idx.close();
      return "";
    }

    std::string key = readWord(idx);
    if (key.empty()) break;

    uint8_t buf[8];
    if (idx.read(buf, 8) != 8) break;

    uint32_t dictOffset = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
                          (static_cast<uint32_t>(buf[2]) << 8) | static_cast<uint32_t>(buf[3]);
    uint32_t dictSize = (static_cast<uint32_t>(buf[4]) << 24) | (static_cast<uint32_t>(buf[5]) << 16) |
                        (static_cast<uint32_t>(buf[6]) << 8) | static_cast<uint32_t>(buf[7]);

    if (key == word) {
      idx.close();
      if (onProgress) onProgress(100);
      return readDefinition(dictOffset, dictSize);
    }

    if (key > word) break;
  }

  idx.close();
  if (onProgress) onProgress(100);
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

std::vector<std::string> Dictionary::findSimilar(const std::string& word, int maxResults) {
  if (!indexLoaded || sparseOffsets.empty()) return {};

  char idxPath[520];
  buildPath(idxPath, sizeof(idxPath), "dict-data.idx");

  FsFile idx;
  if (!Storage.openFileForRead("DICT", idxPath, idx)) return {};

  int lo = 0, hi = static_cast<int>(sparseOffsets.size()) - 1;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    idx.seekSet(sparseOffsets[mid]);
    std::string key = readWord(idx);
    if (key <= word) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  int startSeg = std::max(0, lo - 1);
  int endSeg = std::min(static_cast<int>(sparseOffsets.size()) - 1, lo + 1);
  idx.seekSet(sparseOffsets[startSeg]);

  int totalToScan = (endSeg - startSeg + 1) * SPARSE_INTERVAL;
  int remaining = static_cast<int>(totalWords) - startSeg * SPARSE_INTERVAL;
  if (totalToScan > remaining) totalToScan = remaining;

  int maxDist = std::max(2, static_cast<int>(word.size()) / 3 + 1);

  struct Candidate {
    std::string text;
    int distance;
  };
  std::vector<Candidate> candidates;

  for (int i = 0; i < totalToScan; i++) {
    std::string key = readWord(idx);
    if (key.empty()) break;

    uint8_t skip[8];
    if (idx.read(skip, 8) != 8) break;

    if (key == word) continue;
    int dist = editDistance(key, word, maxDist);
    if (dist <= maxDist) {
      candidates.push_back({key, dist});
    }
  }

  idx.close();

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

  std::vector<std::string> results;
  for (size_t i = 0; i < candidates.size() && static_cast<int>(results.size()) < maxResults; i++) {
    results.push_back(candidates[i].text);
  }
  return results;
}
