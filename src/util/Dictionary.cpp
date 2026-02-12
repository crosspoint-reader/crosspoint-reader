#include "Dictionary.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>

namespace {
constexpr const char* DICT_PATH = "/dictionary.json";
constexpr size_t READ_BUF_SIZE = 256;
constexpr size_t NPOS = SIZE_MAX;
}  // namespace

bool Dictionary::exists() { return Storage.exists(DICT_PATH); }

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
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

// Scan forward from `pos` to find the opening `"` of the next JSON key.
// A key follows `{` or `,` (with optional whitespace). Returns NPOS if not found before fileSize.
size_t Dictionary::seekToNearestKey(FsFile& file, size_t pos, size_t fileSize) {
  if (pos >= fileSize) return NPOS;
  file.seekSet(pos);

  uint8_t buf[READ_BUF_SIZE];
  size_t currentPos = pos;
  bool afterSep = false;

  while (currentPos < fileSize) {
    size_t toRead = std::min(static_cast<size_t>(READ_BUF_SIZE), fileSize - currentPos);
    int bytesRead = file.read(buf, toRead);
    if (bytesRead <= 0) break;

    for (int i = 0; i < bytesRead; i++) {
      char c = static_cast<char>(buf[i]);
      if (!afterSep) {
        if (c == ',' || c == '{') afterSep = true;
      } else {
        if (c == '"') return currentPos + i;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') afterSep = false;
      }
    }
    currentPos += bytesRead;
  }
  return NPOS;
}

// Read the JSON key string starting at `pos` (the opening `"`).
// Leaves the file positioned right after the closing `"`.
std::string Dictionary::readKeyAt(FsFile& file, size_t pos) {
  file.seekSet(pos);
  if (file.read() != '"') return "";

  std::string key;
  key.reserve(32);
  while (true) {
    int ch = file.read();
    if (ch < 0 || ch == '"') break;
    if (ch == '\\') {
      ch = file.read();
      if (ch < 0) break;
    }
    key += static_cast<char>(ch);
  }
  return key;
}

// Read the JSON string value after a key. Expects the file positioned right after the
// key's closing `"`. Skips whitespace, colon, whitespace, then reads the quoted value.
std::string Dictionary::extractDefinition(FsFile& file) {
  int ch;
  do {
    ch = file.read();
  } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
  if (ch != ':') return "";

  do {
    ch = file.read();
  } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
  if (ch != '"') return "";

  std::string def;
  bool escaped = false;
  while (true) {
    ch = file.read();
    if (ch < 0) break;
    if (escaped) {
      switch (ch) {
        case 'n':
          def += '\n';
          break;
        case '"':
          def += '"';
          break;
        case '\\':
          def += '\\';
          break;
        case 't':
          def += '\t';
          break;
        default:
          def += static_cast<char>(ch);
          break;
      }
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      break;
    } else {
      def += static_cast<char>(ch);
    }
  }
  return def;
}

std::string Dictionary::lookup(const std::string& word, const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  FsFile file;
  if (!Storage.openFileForRead("DICT", DICT_PATH, file)) return "";

  const size_t fileSize = static_cast<size_t>(file.fileSize());
  if (fileSize == 0) {
    file.close();
    return "";
  }

  size_t lo = 0, hi = fileSize;
  constexpr int MAX_ITER = 50;

  // Binary search: seek to midpoint, find nearest key, compare
  for (int iter = 0; lo < hi && iter < MAX_ITER; iter++) {
    if (shouldCancel && shouldCancel()) {
      file.close();
      return "";
    }

    if (onProgress) onProgress(std::min(90, iter * 90 / 20));

    size_t mid = lo + (hi - lo) / 2;
    size_t keyPos = seekToNearestKey(file, mid, fileSize);

    if (keyPos == NPOS || keyPos >= hi) {
      hi = mid;
      continue;
    }

    std::string key = readKeyAt(file, keyPos);
    if (key.empty()) {
      hi = mid;
      continue;
    }

    // Verify this is a real key (followed by ':' after optional whitespace)
    size_t afterKey = static_cast<size_t>(file.curPosition());
    int ch;
    do {
      ch = file.read();
    } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');

    if (ch != ':') {
      // Not a real key — likely inside a value string. Advance past it.
      lo = mid + 1;
      continue;
    }

    int cmp = key.compare(word);
    if (cmp == 0) {
      // Found the word. Seek back to after the key's closing quote and extract definition.
      file.seekSet(afterKey);
      std::string def = extractDefinition(file);
      file.close();
      if (onProgress) onProgress(100);
      return def;
    } else if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  // Linear fallback: scan a region around the convergence point
  size_t scanStart = (lo > 2048) ? lo - 2048 : 0;
  size_t scanEnd = std::min(lo + 8192, fileSize);
  size_t pos = seekToNearestKey(file, scanStart, fileSize);

  while (pos != NPOS && pos < scanEnd) {
    if (shouldCancel && shouldCancel()) {
      file.close();
      return "";
    }

    std::string key = readKeyAt(file, pos);
    if (key.empty()) break;

    // Verify real key
    size_t afterKey = static_cast<size_t>(file.curPosition());
    int ch;
    do {
      ch = file.read();
    } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');

    if (ch == ':') {
      if (key == word) {
        file.seekSet(afterKey);
        std::string def = extractDefinition(file);
        file.close();
        if (onProgress) onProgress(100);
        return def;
      }
      if (key > word) break;
    }

    pos = seekToNearestKey(file, static_cast<size_t>(file.curPosition()), fileSize);
  }

  file.close();
  if (onProgress) onProgress(100);
  return "";
}
