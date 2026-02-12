#include "Dictionary.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>

namespace {
constexpr const char* DICT_PATH = "/dictionary.json";
constexpr size_t READ_BUF_SIZE = 256;
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

std::string Dictionary::lookup(const std::string& word, const std::function<void(int percent)>& onProgress) {
  FsFile file;
  if (!Storage.openFileForRead("DICT", DICT_PATH, file)) {
    return "";
  }

  const size_t fileSize = static_cast<size_t>(file.available());
  size_t bytesReadTotal = 0;
  int lastReportedPercent = 0;

  auto reportProgress = [&]() {
    if (onProgress && fileSize > 0) {
      int percent = static_cast<int>(bytesReadTotal * 100 / fileSize);
      if (percent >= lastReportedPercent + 10) {
        lastReportedPercent = percent;
        onProgress(percent);
      }
    }
  };

  // Build search key: "word" : or "word":
  const std::string searchKey = "\"" + word + "\"";

  char buf[READ_BUF_SIZE];
  // Overlap region: keep tail of previous chunk to handle keys split across boundaries
  constexpr size_t OVERLAP = 128;
  std::string carryOver;

  while (true) {
    int bytesRead = file.read(reinterpret_cast<uint8_t*>(buf), READ_BUF_SIZE);
    if (bytesRead <= 0) break;

    bytesReadTotal += bytesRead;
    reportProgress();

    std::string chunk = carryOver + std::string(buf, bytesRead);

    size_t pos = chunk.find(searchKey);
    while (pos != std::string::npos) {
      // Check that this looks like a JSON key (preceded by start-of-object or comma+whitespace)
      // and followed by optional whitespace + colon
      size_t afterKey = pos + searchKey.size();

      // Find colon after the key
      size_t colonPos = afterKey;
      while (colonPos < chunk.size() && (chunk[colonPos] == ' ' || chunk[colonPos] == '\t')) {
        colonPos++;
      }

      bool needMoreData = false;

      if (colonPos >= chunk.size()) {
        // Need more data to see the colon
        needMoreData = true;
      } else if (chunk[colonPos] == ':') {
        // Found a match! Now extract the definition string value.
        // Skip colon and whitespace to find the opening quote
        size_t valStart = colonPos + 1;
        while (valStart < chunk.size() && (chunk[valStart] == ' ' || chunk[valStart] == '\t')) {
          valStart++;
        }

        if (valStart >= chunk.size()) {
          needMoreData = true;
        } else if (chunk[valStart] == '"') {
          // Read the definition value, handling escaped characters
          std::string definition;
          size_t i = valStart + 1;
          bool escaped = false;
          bool complete = false;

          while (!complete) {
            while (i < chunk.size()) {
              char c = chunk[i];
              if (escaped) {
                if (c == 'n') {
                  definition += '\n';
                } else if (c == '"') {
                  definition += '"';
                } else if (c == '\\') {
                  definition += '\\';
                } else if (c == 't') {
                  definition += '\t';
                } else {
                  definition += c;
                }
                escaped = false;
              } else if (c == '\\') {
                escaped = true;
              } else if (c == '"') {
                complete = true;
                break;
              } else {
                definition += c;
              }
              i++;
            }

            if (!complete) {
              // Need to read more data
              int more = file.read(reinterpret_cast<uint8_t*>(buf), READ_BUF_SIZE);
              if (more <= 0) {
                // Unexpected end of file
                file.close();
                return definition;
              }
              bytesReadTotal += more;
              chunk += std::string(buf, more);
            }
          }

          file.close();
          return definition;
        }
      }

      if (needMoreData) {
        // Read more data and retry from current position
        int more = file.read(reinterpret_cast<uint8_t*>(buf), READ_BUF_SIZE);
        if (more <= 0) {
          file.close();
          return "";
        }
        bytesReadTotal += more;
        chunk += std::string(buf, more);
        // Retry from same position
        continue;
      }

      // Not a real key:value match, search for next occurrence
      pos = chunk.find(searchKey, pos + 1);
    }

    // Keep the tail as overlap for next iteration
    if (chunk.size() > OVERLAP) {
      carryOver = chunk.substr(chunk.size() - OVERLAP);
    } else {
      carryOver = chunk;
    }
  }

  file.close();
  return "";
}
