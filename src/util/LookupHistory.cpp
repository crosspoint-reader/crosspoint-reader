#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string LookupHistory::filePath(const std::string& cachePath) { return cachePath + "/lookups.txt"; }

// Read all entries from the file (oldest first). Returns empty vector on error.
std::vector<LookupHistory::Entry> LookupHistory::readAll(const std::string& path) {
  std::vector<Entry> entries;

  FsFile file;
  if (!Storage.openFileForRead("LH", path.c_str(), file)) return entries;

  char lineBuf[256];
  int lineLen = 0;

  while (file.available()) {
    int b = file.read();
    if (b < 0) break;

    if (b == '\n' || b == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        // Find last '|' separator
        int sepIdx = -1;
        for (int i = lineLen - 1; i >= 0; i--) {
          if (lineBuf[i] == '|') {
            sepIdx = i;
            break;
          }
        }
        Entry e;
        if (sepIdx >= 0 && sepIdx + 1 < lineLen) {
          e.word = std::string(lineBuf, sepIdx);
          char code = lineBuf[sepIdx + 1];
          switch (code) {
            case 'D':
              e.status = Status::Direct;
              break;
            case 'T':
              e.status = Status::Stem;
              break;
            case 'Y':
              e.status = Status::AltForm;
              break;
            case 'S':
              e.status = Status::Suggestion;
              break;
            default:
              e.status = Status::NotFound;
              break;
          }
        } else {
          // No separator — legacy line, treat as not-found status
          e.word = std::string(lineBuf, lineLen);
          e.status = Status::NotFound;
        }
        if (!e.word.empty()) entries.push_back(std::move(e));
        lineLen = 0;
      }
      continue;
    }

    if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) {
      lineBuf[lineLen++] = static_cast<char>(b);
    }
  }

  // Handle last line without trailing newline
  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    int sepIdx = -1;
    for (int i = lineLen - 1; i >= 0; i--) {
      if (lineBuf[i] == '|') {
        sepIdx = i;
        break;
      }
    }
    Entry e;
    if (sepIdx >= 0 && sepIdx + 1 < lineLen) {
      e.word = std::string(lineBuf, sepIdx);
      char code = lineBuf[sepIdx + 1];
      switch (code) {
        case 'D':
          e.status = Status::Direct;
          break;
        case 'T':
          e.status = Status::Stem;
          break;
        case 'Y':
          e.status = Status::AltForm;
          break;
        case 'S':
          e.status = Status::Suggestion;
          break;
        default:
          e.status = Status::NotFound;
          break;
      }
    } else {
      e.word = std::string(lineBuf, lineLen);
      e.status = Status::NotFound;
    }
    if (!e.word.empty()) entries.push_back(std::move(e));
  }

  file.close();
  return entries;
}

// Write all entries (oldest first) to the file, replacing existing content.
bool LookupHistory::writeAll(const std::string& path, const std::vector<Entry>& entries) {
  FsFile file;
  if (!Storage.openFileForWrite("LH", path.c_str(), file)) {
    LOG_ERR("LH", "Failed to open for write: %s", path.c_str());
    return false;
  }
  for (const auto& e : entries) {
    file.write(e.word.c_str(), e.word.size());
    const char pipe = '|';
    file.write(&pipe, 1);
    const char code = static_cast<char>(e.status);
    file.write(&code, 1);
    const char nl = '\n';
    file.write(&nl, 1);
  }
  file.close();
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int LookupHistory::addWord(const std::string& cachePath, const std::string& word, Status status) {
  if (word.empty()) return 0;

  const std::string path = filePath(cachePath);
  auto entries = readAll(path);

  Entry e;
  e.word = word;
  e.status = status;
  entries.push_back(std::move(e));

  // Evict oldest entries if over cap
  const int cap = SETTINGS.getLookupHistoryCapValue();
  while (static_cast<int>(entries.size()) > cap) {
    entries.erase(entries.begin());
  }

  writeAll(path, entries);
  return static_cast<int>(entries.size());
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

bool LookupHistory::hasHistory(const std::string& cachePath) {
  const std::string path = filePath(cachePath);
  if (!Storage.exists(path.c_str())) return false;
  const auto entries = readAll(path);
  return !entries.empty();
}
