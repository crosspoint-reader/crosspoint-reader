#include "VocabularyStore.h"

#include <HalClock.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <common/FsApiConstants.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "util/Dictionary.h"
#include "util/HtmlToPlainText.h"

namespace {
constexpr size_t MAX_WORD_BYTES = 160;
constexpr size_t MAX_DEFINITION_BYTES = 2048;
constexpr size_t MAX_HTML_INPUT_BYTES = 4096;
constexpr size_t MAX_HTML_SCAN_BYTES = 16 * 1024;
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
        // The shared decoder expects terminated input. Copy at most four bytes
        // so a truncated field cannot make it read beyond the input budget.
        unsigned char sequence[5] = {};
        const size_t available = std::min<size_t>(4, size - i);
        for (size_t j = 0; j < available; ++j) sequence[j] = static_cast<unsigned char>(text[i + j]);
        const unsigned char* cursor = sequence;
        const uint32_t codepoint = utf8NextCodepoint(&cursor);
        char encoded[4];
        const char* end = utf8AppendCodepoint(encoded, codepoint);
        const size_t encodedLength = static_cast<size_t>(end - encoded);
        if (encodedLength > maxBytes - encodedBytes) break;
        encodedBytes += encodedLength;
        for (const char* p = encoded; p < end; ++p) put(*p);
        i += static_cast<size_t>(cursor - sequence) - 1;
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

// Scan a bounded HTML window without copying it, stopping at a complete tag
// or entity boundary. A '>' inside a quoted attribute does not end a tag.
// Optionally return the first non-whitespace byte outside markup.
size_t safeHtmlPrefixBytes(const std::string& source, const size_t start, const size_t budget,
                           size_t* firstVisible = nullptr) {
  const size_t limit = start + utf8SafeTruncateBuffer(source.data() + start, std::min(source.size() - start, budget));
  size_t safeEnd = start;
  for (size_t i = start; i < limit;) {
    const char c = source[i];
    if (c == '<') {
      if (i + 1 == limit && limit < source.size()) break;
      if (htmlTagStart(source, i)) {
        const size_t close = htmlTagEnd(source, i, limit);
        if (close == std::string::npos) break;
        safeEnd = close + 1;
        i = close + 1;
        continue;
      }
    }
    if (firstVisible && *firstVisible == std::string::npos && !std::isspace(static_cast<unsigned char>(c)))
      *firstVisible = i;
    safeEnd = ++i;
  }
  if (safeEnd > start && safeEnd < source.size()) {
    const size_t entityStart = source.rfind('&', safeEnd - 1);
    const size_t entityEnd = source.rfind(';', safeEnd - 1);
    if (entityStart != std::string::npos && entityStart >= start &&
        (entityEnd == std::string::npos || entityStart > entityEnd) && safeEnd - entityStart <= 18) {
      safeEnd = entityStart;
    }
  }
  return safeEnd;
}

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

  // Walk backward in bounded chunks even when an externally written tail is
  // longer than a firmware record. recoverTail preserves complete objects and
  // discards invalid partial tails after the last newline.
  constexpr size_t CHUNK_BYTES = 128;
  char chunk[CHUNK_BYTES];
  size_t cursor = size;
  while (cursor > 0) {
    const size_t count = std::min(cursor, CHUNK_BYTES);
    cursor -= count;
    if (!file.seek(cursor) || file.read(chunk, count) != static_cast<int>(count)) return false;
    for (size_t i = count; i > 0; --i) {
      if (chunk[i - 1] == '\n') {
        const size_t newSize = cursor + i;
        return recoverTail(file, newSize, size);
      }
    }
  }
  return recoverTail(file, 0, size);
}
}  // namespace

bool VocabularyStore::append(const VocabularyContext& context, const std::string& word, const std::string& headword,
                             const std::string& definition, const bool htmlDefinition) {
  if (word.empty() || definition.empty()) return false;

  std::string plain;
  const std::string* exportedDefinition = &definition;
  bool sourceTruncated = false;
  if (htmlDefinition) {
    // Find the first text within 16 KiB of markup, but keep the actual HTML
    // conversion bounded to 4 KiB even on low-memory devices.
    size_t firstVisible = std::string::npos;
    safeHtmlPrefixBytes(definition, 0, MAX_HTML_SCAN_BYTES, &firstVisible);
    if (firstVisible == std::string::npos) return false;
    const size_t initialEnd = safeHtmlPrefixBytes(definition, 0, MAX_HTML_INPUT_BYTES);
    const size_t start = firstVisible < initialEnd ? 0 : firstVisible;
    const size_t end = start == 0 ? initialEnd : safeHtmlPrefixBytes(definition, start, MAX_HTML_INPUT_BYTES);
    if (end <= start) return false;
    const size_t bytes = end - start;
    sourceTruncated = end < definition.size();
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.largestBlockBytes < bytes + 1 || heap.freeBytes < bytes + 8 * 1024) {
      LOG_ERR("VOCAB", "Not enough memory to export formatted definition");
      return false;
    }
    plain = htmlToPlainText(definition.substr(start, bytes), DICTIONARY_PLAIN_HEADWORD_MARKER);
    std::replace(plain.begin(), plain.end(), DICTIONARY_PLAIN_HEADWORD_MARKER, ' ');
    const bool visible =
        std::any_of(plain.begin(), plain.end(), [](const unsigned char c) { return !std::isspace(c); });
    if (!visible) return false;
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
    snprintf(number, sizeof(number), "%.7g", static_cast<double>(*context.seriesIndex));
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
