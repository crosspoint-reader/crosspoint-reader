#include "Dictionary.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr const char* IDX_PATH = "/dictionary.idx";
constexpr const char* DICT_PATH = "/dictionary.dict";
constexpr const char* CACHE_PATH = "/dictionary.cache";
constexpr uint32_t CACHE_MAGIC = 0x44494354;  // "DICT"
}  // namespace

std::vector<uint32_t> Dictionary::sparseOffsets;
uint32_t Dictionary::totalWords = 0;
bool Dictionary::indexLoaded = false;

bool Dictionary::exists() { return Storage.exists(IDX_PATH); }

std::string Dictionary::cleanWord(const std::string& word) {
  if (word.empty()) return "";

  // Find first alphanumeric character
  size_t start = 0;
  while (start < word.size() && !std::isalnum(static_cast<unsigned char>(word[start]))) {
    start++;
  }

  // Find last alphanumeric character
  size_t end = word.size();
  while (end > start && !std::isalnum(static_cast<unsigned char>(word[end - 1]))) {
    end--;
  }

  if (start >= end) return "";

  std::string result = word.substr(start, end - start);
  // Lowercase
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
  return result;
}

bool Dictionary::loadCachedIndex() {
  FsFile cache;
  if (!Storage.openFileForRead("DICT", CACHE_PATH, cache)) return false;

  // Read and validate header
  uint8_t header[16];
  if (cache.read(header, 16) != 16) {
    cache.close();
    return false;
  }

  uint32_t magic = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                   (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
  uint32_t expectedIdxSize = (static_cast<uint32_t>(header[4]) << 24) | (static_cast<uint32_t>(header[5]) << 16) |
                             (static_cast<uint32_t>(header[6]) << 8) | static_cast<uint32_t>(header[7]);
  uint32_t cachedTotalWords = (static_cast<uint32_t>(header[8]) << 24) | (static_cast<uint32_t>(header[9]) << 16) |
                              (static_cast<uint32_t>(header[10]) << 8) | static_cast<uint32_t>(header[11]);
  uint32_t offsetCount = (static_cast<uint32_t>(header[12]) << 24) | (static_cast<uint32_t>(header[13]) << 16) |
                         (static_cast<uint32_t>(header[14]) << 8) | static_cast<uint32_t>(header[15]);

  if (magic != CACHE_MAGIC) {
    cache.close();
    return false;
  }

  // Validate against actual .idx file size
  FsFile idx;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idx)) {
    cache.close();
    return false;
  }
  uint32_t actualIdxSize = static_cast<uint32_t>(idx.fileSize());
  idx.close();

  if (expectedIdxSize != actualIdxSize || offsetCount == 0) {
    cache.close();
    return false;
  }

  // Read sparse offsets
  sparseOffsets.resize(offsetCount);
  int bytesNeeded = static_cast<int>(offsetCount * 4);
  int bytesRead = cache.read(reinterpret_cast<uint8_t*>(sparseOffsets.data()), bytesNeeded);
  cache.close();

  if (bytesRead != bytesNeeded) {
    sparseOffsets.clear();
    return false;
  }

  totalWords = cachedTotalWords;
  indexLoaded = true;
  return true;
}

void Dictionary::saveCachedIndex() {
  FsFile idx;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idx)) return;
  uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());
  idx.close();

  FsFile cache;
  if (!Storage.openFileForWrite("DICT", CACHE_PATH, cache)) return;

  uint32_t offsetCount = static_cast<uint32_t>(sparseOffsets.size());

  // Write header: magic, idx file size, totalWords, offset count (all big-endian)
  uint8_t header[16];
  header[0] = (CACHE_MAGIC >> 24) & 0xFF;
  header[1] = (CACHE_MAGIC >> 16) & 0xFF;
  header[2] = (CACHE_MAGIC >> 8) & 0xFF;
  header[3] = CACHE_MAGIC & 0xFF;
  header[4] = (idxSize >> 24) & 0xFF;
  header[5] = (idxSize >> 16) & 0xFF;
  header[6] = (idxSize >> 8) & 0xFF;
  header[7] = idxSize & 0xFF;
  header[8] = (totalWords >> 24) & 0xFF;
  header[9] = (totalWords >> 16) & 0xFF;
  header[10] = (totalWords >> 8) & 0xFF;
  header[11] = totalWords & 0xFF;
  header[12] = (offsetCount >> 24) & 0xFF;
  header[13] = (offsetCount >> 16) & 0xFF;
  header[14] = (offsetCount >> 8) & 0xFF;
  header[15] = offsetCount & 0xFF;
  cache.write(header, 16);

  // Write sparse offsets (native endian — same device always reads back)
  cache.write(reinterpret_cast<const uint8_t*>(sparseOffsets.data()), offsetCount * 4);
  cache.close();
}

// Scan the .idx file to build a sparse offset table for fast lookups.
// Records the file offset of every SPARSE_INTERVAL-th entry.
bool Dictionary::loadIndex(const std::function<void(int percent)>& onProgress,
                           const std::function<bool()>& shouldCancel) {
  FsFile idx;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idx)) return false;

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

    // Skip word (read until null terminator)
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
  if (totalWords > 0) {
    saveCachedIndex();
  }
  return totalWords > 0;
}

// Read a null-terminated word string from the current file position.
std::string Dictionary::readWord(FsFile& file) {
  std::string word;
  while (true) {
    int ch = file.read();
    if (ch <= 0) break;  // null terminator (0) or error (-1)
    word += static_cast<char>(ch);
  }
  return word;
}

// Read a definition from the .dict file at the given offset and size.
std::string Dictionary::readDefinition(uint32_t offset, uint32_t size) {
  FsFile dict;
  if (!Storage.openFileForRead("DICT", DICT_PATH, dict)) return "";

  dict.seekSet(offset);

  std::string def(size, '\0');
  int bytesRead = dict.read(reinterpret_cast<uint8_t*>(&def[0]), size);
  dict.close();

  if (bytesRead < 0) return "";
  if (static_cast<uint32_t>(bytesRead) < size) def.resize(bytesRead);
  return def;
}

std::string Dictionary::lookup(const std::string& word, const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  if (!indexLoaded) {
    if (!loadCachedIndex()) {
      if (!loadIndex(onProgress, shouldCancel)) return "";
    }
  }

  if (sparseOffsets.empty()) return "";

  FsFile idx;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idx)) return "";

  // Binary search the sparse offset table to find the right segment.
  // Find the rightmost segment whose first word is <= the search word.
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

  // Linear scan within the segment starting at sparseOffsets[lo].
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

    // Read offset and size (4 bytes each, big-endian)
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

    if (key > word) break;  // Past the word alphabetically
  }

  idx.close();
  if (onProgress) onProgress(100);
  return "";
}

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

  // Plurals (longer suffixes first to avoid partial matches)
  if (endsWith("sses")) add(word.substr(0, len - 2));
  if (endsWith("ies")) {
    add(word.substr(0, len - 3) + "y");
    if (len == 4) add(word.substr(0, len - 1));
  }
  if (endsWith("ves")) {
    add(word.substr(0, len - 3) + "f");
    add(word.substr(0, len - 3) + "fe");
  }
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
  if (endsWith("ily")) {
    add(word.substr(0, len - 3) + "y");
  }
  if (endsWith("ly") && !endsWith("ily")) {
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
  if (endsWith("tion")) add(word.substr(0, len - 4) + "te");
  if (endsWith("ation")) add(word.substr(0, len - 5) + "e");

  // Prefix removal
  if (len > 5 && word.compare(0, 2, "un") == 0) add(word.substr(2));
  if (len > 6 && word.compare(0, 3, "dis") == 0) add(word.substr(3));
  if (len > 5 && word.compare(0, 2, "re") == 0) add(word.substr(2));

  // Deduplicate
  std::sort(variants.begin(), variants.end());
  variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
  return variants;
}

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

  FsFile idx;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idx)) return {};

  // Binary search to find the segment containing or nearest to the word
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

  // Scan entries from the segment before through the segment after the target
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
