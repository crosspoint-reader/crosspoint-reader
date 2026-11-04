#include "VocabularyStore.h"

#include <HalClock.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <common/FsApiConstants.h>
#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "util/Dictionary.h"
#include "util/HtmlToPlainText.h"

namespace {
constexpr size_t MAX_WORD_BYTES = 160;
constexpr size_t MAX_DEFINITION_BYTES = 2048;
constexpr size_t MAX_HTML_INPUT_BYTES = 4096;
constexpr size_t MAX_TITLE_BYTES = 256;
constexpr size_t MAX_METADATA_BYTES = 160;

// Buffered output avoids allocating a full JSON record on the X3.
class RecordWriter {
 public:
  explicit RecordWriter(HalFile& output) : file(output) {}

  void raw(const char* text) {
    while (*text) put(*text++);
  }

  void number(uint32_t value) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
    raw(digits);
  }

  bool quoted(const std::string& text, size_t maxBytes, size_t maxInputBytes = static_cast<size_t>(-1)) {
    put('"');
    const size_t limit = std::min({text.size(), maxBytes, maxInputBytes});
    const size_t size = limit < text.size() ? utf8SafeTruncateBuffer(text.data(), limit) : limit;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    size_t encodedBytes = 0;
    size_t i = 0;
    for (; i < size; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c >= 0x80) {
        const size_t count = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool valid = count != 0 && count <= size - i;
        for (size_t j = 1; valid && j < count; ++j) {
          if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) valid = false;
        }
        if (valid && count == 3) {
          const auto second = static_cast<unsigned char>(text[i + 1]);
          if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0)) valid = false;
        }
        if (valid && count == 4) {
          const auto second = static_cast<unsigned char>(text[i + 1]);
          if ((c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) valid = false;
        }
        const size_t encodedLength = valid ? count : 3;
        if (encodedLength > maxBytes - encodedBytes) break;
        encodedBytes += encodedLength;
        if (valid) {
          for (size_t j = 0; j < count; ++j) put(text[i + j]);
          i += count - 1;
        } else {
          put(static_cast<char>(0xEF));
          put(static_cast<char>(0xBF));
          put(static_cast<char>(0xBD));
        }
        continue;
      }
      if (encodedBytes == maxBytes) break;
      ++encodedBytes;
      if (c == '"' || c == '\\') {
        put('\\');
        put(static_cast<char>(c));
      } else if (c < 0x20) {
        raw("\\u00");
        put(HEX_DIGITS[c >> 4]);
        put(HEX_DIGITS[c & 0xf]);
      } else {
        put(static_cast<char>(c));
      }
    }
    put('"');
    return i < text.size();
  }

  bool finish() { return flush() && good; }

 private:
  static constexpr size_t BUFFER_BYTES = 192;
  HalFile& file;
  char buffer[BUFFER_BYTES] = {};
  size_t used = 0;
  bool good = true;

  void put(char c) {
    if (!good) return;
    buffer[used++] = c;
    if (used == sizeof(buffer)) flush();
  }

  bool flush() {
    if (good && used != 0) good = file.write(buffer, used) == used;
    used = 0;
    return good;
  }
};

time_t validLookupTime() {
  struct tm local = {};
  if (halClock.localTime(local) && local.tm_year >= 120) return mktime(&local);
  const time_t systemTime = time(nullptr);
  return systemTime >= 1577836800 ? systemTime : 0;
}
// A bounded structural check lets us preserve a final JSON object without a newline.
// Vocabulary records have a flat JSON schema; the check also respects quoted braces.
bool isCompleteJsonObject(HalFile& file, const size_t start, const size_t end) {
  if (start >= end || !file.seek(start)) return false;
  char buffer[128];
  char nesting[8] = {};
  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  bool complete = false;
  for (size_t position = start; position < end;) {
    const size_t bytes = std::min(end - position, sizeof(buffer));
    if (file.read(buffer, bytes) != static_cast<int>(bytes)) return false;
    position += bytes;
    for (size_t i = 0; i < bytes; ++i) {
      const char c = buffer[i];
      if (complete) {
        if (c != ' ' && c != '\t' && c != '\r') return false;
      } else if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        } else if (static_cast<unsigned char>(c) < 0x20) {
          return false;
        }
      } else if (c == '"') {
        if (depth == 0) return false;
        inString = true;
      } else if (c == '{' || c == '[') {
        if (depth == 0 && (c != '{' || position - bytes + i != start)) return false;
        if (depth >= sizeof(nesting)) return false;
        nesting[depth++] = c;
      } else if (c == '}' || c == ']') {
        if (depth == 0 || nesting[depth - 1] != (c == '}' ? '{' : '[')) return false;
        if (--depth == 0) complete = true;
      } else if (depth == 0 || static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
    }
  }
  return complete && !inString && !escaped;
}
bool recoverTail(HalFile& file, const size_t start, const size_t size) {
  if (isCompleteJsonObject(file, start, size)) {
    return file.seek(size) && file.write(static_cast<uint8_t>('\n')) == 1;
  }
  return file.truncate(start) && file.seek(start);
}
bool recoverIncompleteRecord(HalFile& file) {
  const size_t size = file.fileSize();
  if (size == 0) return true;
  if (!file.seek(size - 1)) return false;
  const int last = file.read();
  if (last < 0) return false;
  if (last == '\n') return file.seek(size);

  // Only a power interruption or failed rollback should leave a partial line.
  // Search a bounded tail to avoid walking a large history byte by byte.
  constexpr size_t CHUNK_BYTES = 128;
  constexpr size_t MAX_RECORD_BYTES = 24 * 1024;
  char chunk[CHUNK_BYTES];
  size_t cursor = size;
  size_t scanned = 0;
  while (cursor > 0 && scanned < MAX_RECORD_BYTES) {
    const size_t count = std::min(std::min(cursor, CHUNK_BYTES), MAX_RECORD_BYTES - scanned);
    cursor -= count;
    if (!file.seek(cursor) || file.read(chunk, count) != static_cast<int>(count)) return false;
    for (size_t i = count; i > 0; --i) {
      if (chunk[i - 1] == '\n') {
        const size_t newSize = cursor + i;
        return recoverTail(file, newSize, size);
      }
    }
    scanned += count;
  }
  if (cursor == 0) return recoverTail(file, 0, size);
  LOG_ERR("VOCAB", "Incomplete vocabulary record exceeds recovery limit");
  return false;
}
}  // namespace

bool VocabularyStore::append(const VocabularyContext& context, const std::string& word, const std::string& headword,
                             const std::string& definition, const bool htmlDefinition) {
  if (word.empty() || definition.empty()) return false;

  std::string plain;
  const std::string* exportedDefinition = &definition;
  bool sourceTruncated = false;
  if (htmlDefinition) {
    // Limit conversion memory even when a dictionary entry is very large.
    size_t bytes = utf8SafeTruncateBuffer(definition.data(), std::min(definition.size(), MAX_HTML_INPUT_BYTES));
    sourceTruncated = bytes < definition.size();
    if (sourceTruncated && bytes > 0) {
      // Avoid interpreting an incomplete trailing HTML tag or entity as text.
      const size_t lastTagStart = definition.rfind('<', bytes - 1);
      const size_t lastTagEnd = definition.rfind('>', bytes - 1);
      if (lastTagStart != std::string::npos && (lastTagEnd == std::string::npos || lastTagStart > lastTagEnd)) {
        bytes = lastTagStart;
      }
      if (bytes > 0) {
        const size_t lastEntityStart = definition.rfind('&', bytes - 1);
        const size_t lastEntityEnd = definition.rfind(';', bytes - 1);
        if (lastEntityStart != std::string::npos &&
            (lastEntityEnd == std::string::npos || lastEntityStart > lastEntityEnd) && bytes - lastEntityStart <= 18) {
          bytes = lastEntityStart;
        }
      }
    }
    if (bytes == 0) return false;
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.largestBlockBytes < bytes + 1 || heap.freeBytes < bytes + 8 * 1024) {
      LOG_ERR("VOCAB", "Not enough memory to export formatted definition");
      return false;
    }
    plain = htmlToPlainText(definition.substr(0, bytes), DICTIONARY_PLAIN_HEADWORD_MARKER);
    std::replace(plain.begin(), plain.end(), DICTIONARY_PLAIN_HEADWORD_MARKER, ' ');
    if (plain.empty()) return false;
    exportedDefinition = &plain;
  }

  HalFile file = Storage.open(FILE_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open vocabulary history");
    return false;
  }
  if (!recoverIncompleteRecord(file)) {
    LOG_ERR("VOCAB", "Failed to recover vocabulary file tail");
    return false;
  }
  const size_t originalSize = file.fileSize();

  char id[33];
  snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  RecordWriter record(file);
  record.raw("{\"v\":1,\"id\":\"");
  record.raw(id);
  record.raw("\",\"timestamp\":");
  const time_t timestamp = validLookupTime();
  if (timestamp > 0) {
    record.number(static_cast<uint32_t>(timestamp));
  } else {
    record.raw("null");
  }
  record.raw(",\"word\":");
  record.quoted(word, MAX_WORD_BYTES);
  record.raw(",\"headword\":");
  record.quoted(headword, MAX_WORD_BYTES);
  record.raw(",\"definition\":");
  const auto& text = *exportedDefinition;
  size_t definitionBytes = std::min(text.size(), MAX_DEFINITION_BYTES);
  if (definitionBytes < text.size()) definitionBytes = utf8SafeTruncateBuffer(text.data(), definitionBytes);
  bool definitionTruncated = sourceTruncated || definitionBytes < text.size();
  if (definitionBytes < text.size() && definitionBytes > 0) {
    const size_t lastLine = text.rfind('\n', definitionBytes - 1);
    if (lastLine != std::string::npos && lastLine >= definitionBytes / 2) {
      definitionBytes = lastLine;
    } else {
      const size_t lastSpace = text.find_last_of(" \t\r", definitionBytes - 1);
      if (lastSpace != std::string::npos && lastSpace >= definitionBytes / 2) definitionBytes = lastSpace;
    }
  }
  if (record.quoted(text, MAX_DEFINITION_BYTES, definitionBytes)) definitionTruncated = true;
  record.raw(",\"definition_truncated\":");
  record.raw(definitionTruncated ? "true" : "false");
  record.raw(",\"title\":");
  record.quoted(context.title, MAX_TITLE_BYTES);
  record.raw(",\"author\":");
  record.quoted(context.author, MAX_METADATA_BYTES);
  record.raw(",\"series\":");
  record.quoted(context.series, MAX_METADATA_BYTES);
  record.raw(",\"series_index\":");
  if (context.seriesIndex && std::isfinite(*context.seriesIndex)) {
    char number[32];
    snprintf(number, sizeof(number), "%.6g", static_cast<double>(*context.seriesIndex));
    record.raw(number);
  } else {
    record.raw("null");
  }
  record.raw(",\"chapter\":");
  record.quoted(context.chapter, MAX_METADATA_BYTES);
  record.raw(",\"page\":");
  record.number(context.page);
  record.raw(",\"spine\":");
  record.number(context.spine);
  record.raw(",\"start_offset\":");
  if (context.startOffset == UINT32_MAX) {
    record.raw("null");
  } else {
    record.number(context.startOffset);
  }
  record.raw("}\n");

  const bool written = record.finish();
  if (!written && !file.truncate(originalSize)) LOG_ERR("VOCAB", "Failed to restore partial vocabulary record");
  const bool closed = file.close();
  if (!closed) LOG_ERR("VOCAB", "Failed to flush vocabulary record");
  if (!written) LOG_ERR("VOCAB", "Could not append vocabulary record");
  return written && closed;
}
