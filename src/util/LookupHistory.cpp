#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <array>

#include "CrossPointSettings.h"

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string LookupHistory::filePath(const std::string& cachePath) { return cachePath + "/dictionary_history.txt"; }

// Parse status code char to Status enum.
static LookupHistory::Status parseStatusCode(char code) {
  switch (code) {
    case 'D':
      return LookupHistory::Status::Direct;
    case 'T':
      return LookupHistory::Status::Stem;
    case 'Y':
      return LookupHistory::Status::AltForm;
    case 'S':
      return LookupHistory::Status::Suggestion;
    default:
      return LookupHistory::Status::NotFound;
  }
}

// Parse a TSV line into an Entry.
static LookupHistory::Entry parseLine(const char* lineBuf, int lineLen) {
  LookupHistory::Entry e;
  std::array<int, 5> tabs{};
  int foundTabs = 0;
  for (int i = 0; i < lineLen && foundTabs < static_cast<int>(tabs.size()); i++) {
    if (lineBuf[i] == '\t') tabs[foundTabs++] = i;
  }
  if (foundTabs != static_cast<int>(tabs.size()) || tabs[0] <= 0 || tabs[0] + 1 >= lineLen) return e;

  e.word = std::string(lineBuf, tabs[0]);
  e.status = parseStatusCode(lineBuf[tabs[0] + 1]);
  e.headword = std::string(lineBuf + tabs[1] + 1, tabs[2] - tabs[1] - 1);
  e.sourceInTempFile = tabs[2] + 1 < lineLen && lineBuf[tabs[2] + 1] == 'T';
  e.sourceOffset =
      static_cast<uint32_t>(strtoul(std::string(lineBuf + tabs[3] + 1, tabs[4] - tabs[3] - 1).c_str(), nullptr, 10));
  e.sourceSize =
      static_cast<uint32_t>(strtoul(std::string(lineBuf + tabs[4] + 1, lineLen - tabs[4] - 1).c_str(), nullptr, 10));
  return e;
}

// Read all entries from the file (oldest first). Returns empty vector on error.
std::vector<LookupHistory::Entry> LookupHistory::readAll(const std::string& path) {
  std::vector<Entry> entries;
  entries.reserve(32);

  HalFile file;
  if (!Storage.openFileForRead("LH", path.c_str(), file)) return entries;

  char lineBuf[256];
  int lineLen = 0;
  bool full = false;

  while (!full && file.available()) {
    int b = file.read();
    if (b < 0) break;

    if (b == '\n' || b == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        Entry e = parseLine(lineBuf, lineLen);
        if (!e.word.empty()) {
          entries.push_back(std::move(e));
          if (entries.size() >= CrossPointSettings::HIST_CAP_MAX) full = true;
        }
        lineLen = 0;
      }
      continue;
    }

    if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) {
      lineBuf[lineLen++] = static_cast<char>(b);
    }
  }

  // Handle last line without trailing newline
  if (!full && lineLen > 0) {
    lineBuf[lineLen] = '\0';
    Entry e = parseLine(lineBuf, lineLen);
    if (!e.word.empty()) entries.push_back(std::move(e));
  }

  file.close();
  return entries;
}

// Write all entries (oldest first) to the file, replacing existing content.
bool LookupHistory::writeAll(const std::string& path, const std::vector<Entry>& entries) {
  HalFile file;
  if (!Storage.openFileForWrite("LH", path.c_str(), file)) {
    LOG_ERR("LH", "Failed to open for write: %s", path.c_str());
    return false;
  }
  for (const auto& e : entries) {
    file.write(e.word.c_str(), e.word.size());
    const char tab = '\t';
    file.write(&tab, 1);
    const char code = static_cast<char>(e.status);
    file.write(&code, 1);
    file.write(&tab, 1);
    file.write(e.headword.c_str(), e.headword.size());
    file.write(&tab, 1);
    const char sourceKind = e.sourceInTempFile ? 'T' : 'D';
    file.write(&sourceKind, 1);
    file.write(&tab, 1);
    const std::string offset = std::to_string(e.sourceOffset);
    file.write(offset.c_str(), offset.size());
    file.write(&tab, 1);
    const std::string size = std::to_string(e.sourceSize);
    file.write(size.c_str(), size.size());
    const char nl = '\n';
    file.write(&nl, 1);
  }
  file.close();
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int LookupHistory::addWord(const std::string& cachePath, const std::string& word, Status status,
                           const std::string& headword, bool sourceInTempFile, uint32_t sourceOffset,
                           uint32_t sourceSize) {
  if (word.empty()) return 0;

  const std::string path = filePath(cachePath);
  auto entries = readAll(path);

  if (!entries.empty() && entries.back().word == word) return static_cast<int>(entries.size());

  Entry e;
  e.word = word;
  e.headword = headword;
  e.status = status;
  e.sourceInTempFile = sourceInTempFile;
  e.sourceOffset = sourceOffset;
  e.sourceSize = sourceSize;
  entries.push_back(std::move(e));

  // Evict oldest entries if over cap
  const int cap = SETTINGS.getLookupHistoryCapValue();
  while (static_cast<int>(entries.size()) > cap) {
    entries.erase(entries.begin());
  }

  writeAll(path, entries);
  return static_cast<int>(entries.size());
}

void LookupHistory::addWordIf(const std::string& cachePath, const std::string& word, Status status, bool enabled,
                              const std::string& headword, bool sourceInTempFile, uint32_t sourceOffset,
                              uint32_t sourceSize) {
  if (!enabled || word.empty() || cachePath.empty()) return;
  addWord(cachePath, word, status, headword, sourceInTempFile, sourceOffset, sourceSize);
}

std::vector<LookupHistory::Entry> LookupHistory::load(const std::string& cachePath) {
  auto entries = readAll(filePath(cachePath));
  std::reverse(entries.begin(), entries.end());
  return entries;
}

std::string LookupHistory::getWordAt(const std::string& cachePath, int index) {
  const auto entries = readAll(filePath(cachePath));
  if (index < 0 || index >= static_cast<int>(entries.size())) return "";
  return entries[index].word;
}

bool LookupHistory::removeAt(const std::string& cachePath, int index) {
  const std::string path = filePath(cachePath);
  auto entries = readAll(path);
  if (index < 0 || index >= static_cast<int>(entries.size())) return false;
  entries.erase(entries.begin() + index);
  return writeAll(path, entries);
}
