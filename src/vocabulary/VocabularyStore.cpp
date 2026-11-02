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

  void quoted(const std::string& text, size_t maxBytes) {
    put('"');
    const size_t size = utf8SafeTruncateBuffer(text.data(), std::min(text.size(), maxBytes));
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
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
        return file.truncate(newSize) && file.seek(newSize);
      }
    }
    scanned += count;
  }
  if (cursor == 0) return file.truncate(0) && file.seek(0);
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
      if (lastTagStart != std::string::npos &&
          (lastTagEnd == std::string::npos || lastTagStart > lastTagEnd)) {
        bytes = lastTagStart;
      }
      if (bytes > 0) {
        const size_t lastEntityStart = definition.rfind('&', bytes - 1);
        const size_t lastEntityEnd = definition.rfind(';', bytes - 1);
        if (lastEntityStart != std::string::npos &&
            (lastEntityEnd == std::string::npos || lastEntityStart > lastEntityEnd) &&
            bytes - lastEntityStart <= 18) {
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
    plain = htmlToPlainText(definition, DICTIONARY_PLAIN_HEADWORD_MARKER, bytes);
    std::replace(plain.begin(), plain.end(), DICTIONARY_PLAIN_HEADWORD_MARKER, ' ');
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
  size_t definitionBytes = utf8SafeTruncateBuffer(text.data(), std::min(text.size(), MAX_DEFINITION_BYTES));
  const bool definitionTruncated = sourceTruncated || definitionBytes < text.size();
  if (definitionTruncated && definitionBytes > 0) {
    const size_t lastLine = text.rfind('\n', definitionBytes - 1);
    if (lastLine != std::string::npos && lastLine >= definitionBytes / 2) {
      definitionBytes = lastLine;
    } else {
      const size_t lastSpace = text.find_last_of(" \t\r", definitionBytes - 1);
      if (lastSpace != std::string::npos && lastSpace >= definitionBytes / 2) definitionBytes = lastSpace;
    }
  }
  record.quoted(text, definitionBytes);
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
  file.flush();
  if (!written) LOG_ERR("VOCAB", "Could not append vocabulary record");
  return written;
}
