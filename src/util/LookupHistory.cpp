#include "LookupHistory.h"

#include <HalStorage.h>

#include <algorithm>

std::string LookupHistory::filePath(const std::string& cachePath) { return cachePath + "/lookups.txt"; }

bool LookupHistory::hasHistory(const std::string& cachePath) {
  FsFile f;
  if (!Storage.openFileForRead("LKH", filePath(cachePath), f)) {
    return false;
  }
  bool nonEmpty = f.available() > 0;
  f.close();
  return nonEmpty;
}

std::vector<std::string> LookupHistory::load(const std::string& cachePath) {
  std::vector<std::string> words;
  FsFile f;
  if (!Storage.openFileForRead("LKH", filePath(cachePath), f)) {
    return words;
  }

  std::string line;
  while (f.available() && static_cast<int>(words.size()) < MAX_ENTRIES) {
    char c;
    if (f.read(reinterpret_cast<uint8_t*>(&c), 1) != 1) break;
    if (c == '\n') {
      if (!line.empty()) {
        words.push_back(line);
        line.clear();
      }
    } else {
      line += c;
    }
  }
  if (!line.empty() && static_cast<int>(words.size()) < MAX_ENTRIES) {
    words.push_back(line);
  }
  f.close();
  return words;
}

void LookupHistory::removeWord(const std::string& cachePath, const std::string& word) {
  if (word.empty()) return;

  auto existing = load(cachePath);

  FsFile f;
  if (!Storage.openFileForWrite("LKH", filePath(cachePath), f)) {
    return;
  }

  for (const auto& w : existing) {
    if (w != word) {
      f.write(reinterpret_cast<const uint8_t*>(w.c_str()), w.size());
      f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    }
  }
  f.close();
}

void LookupHistory::addWord(const std::string& cachePath, const std::string& word) {
  if (word.empty()) return;

  // Check if already present
  auto existing = load(cachePath);
  if (std::any_of(existing.begin(), existing.end(), [&word](const std::string& w) { return w == word; })) return;

  // Cap at max entries
  if (static_cast<int>(existing.size()) >= MAX_ENTRIES) return;

  FsFile f;
  if (!Storage.openFileForWrite("LKH", filePath(cachePath), f)) {
    return;
  }

  // Rewrite existing entries plus new one
  for (const auto& w : existing) {
    f.write(reinterpret_cast<const uint8_t*>(w.c_str()), w.size());
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  }
  f.write(reinterpret_cast<const uint8_t*>(word.c_str()), word.size());
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.close();
}
