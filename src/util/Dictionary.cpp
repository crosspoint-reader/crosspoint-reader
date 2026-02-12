#include "Dictionary.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>

namespace {
constexpr const char* IDX_PATH = "/dictionary.idx";
constexpr const char* DICT_PATH = "/dictionary.dict";
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
    if (!loadIndex(onProgress, shouldCancel)) return "";
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
