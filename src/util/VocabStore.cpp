#include "VocabStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string_view>

#include "HighlightStore.h"

namespace {

// ASCII-only case folding: enough to match a headword against the entry's own
// first line, which comes from the same dictionary and so agrees byte-for-byte
// on any non-ASCII script.
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    if (ca != cb) return false;
  }
  return true;
}

}  // namespace

namespace VocabStore {

bool save(const std::string& word, const std::string& definition) {
  if (word.empty()) return false;

  // Dictionary entries routinely end in blank lines; they would compound with
  // the separator written below, so trim them off the tail.
  const size_t end = definition.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) return false;
  size_t begin = definition.find_first_not_of(" \t\r\n");

  // Most StarDict entries open with the headword on a line of its own. Under
  // the "## word" heading written below that reads as the word twice, so drop
  // that line — but only when it is nothing but the headword, so a first line
  // carrying pronunciation or part of speech ("word /wɜːd/ n.") is kept whole.
  const size_t firstLineEnd = definition.find('\n', begin);
  if (firstLineEnd != std::string::npos && firstLineEnd < end) {
    std::string_view firstLine(definition.data() + begin, firstLineEnd - begin);
    while (!firstLine.empty() && (firstLine.back() == ' ' || firstLine.back() == '\t' || firstLine.back() == '\r')) {
      firstLine.remove_suffix(1);
    }
    if (equalsIgnoreCaseAscii(firstLine, word)) {
      const size_t next = definition.find_first_not_of(" \t\r\n", firstLineEnd);
      if (next != std::string::npos && next <= end) begin = next;
    }
  }

  const std::string_view body(definition.data() + begin, end + 1 - begin);

  if (!Storage.ensureDirectoryExists(HighlightStore::HIGHLIGHTS_DIR)) {
    LOG_ERR("VOCAB", "Cannot create %s", HighlightStore::HIGHLIGHTS_DIR);
    return false;
  }

  HalFile file = Storage.open(VOCAB_FILE_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open %s", VOCAB_FILE_PATH);
    return false;
  }

  // Only the heading is built in RAM (tens of bytes); the definition is
  // written straight from the caller's string. A single combined buffer would
  // duplicate a multi-KB entry on a 380 KB-RAM device for no gain beyond
  // saving two write calls.
  std::string head;
  head.reserve(word.size() + 24);
  if (file.fileSize() == 0) head += "# Vocabulary\n";
  head += "\n## ";
  head += word;
  head += "\n\n";

  if (file.write(head.data(), head.size()) != head.size() || file.write(body.data(), body.size()) != body.size() ||
      file.write("\n", 1) != 1) {
    LOG_ERR("VOCAB", "Write failed: %s", VOCAB_FILE_PATH);
    return false;
  }
  return true;
}

}  // namespace VocabStore
