#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

// Candidate SD card root directories for dictionaries, checked in priority order.
// The first directory found on the SD card is used; the rest are ignored.
static constexpr const char* DICT_ROOT_CANDIDATES[] = {
    "/.dictionaries",
    "/dictionaries",
};

bool DictionaryRegistry::discover() {
  entries_.clear();
  entries_.reserve(16);
  root_.clear();

  for (const auto* candidate : DICT_ROOT_CANDIDATES) {
    auto dir = Storage.open(candidate);
    if (dir && dir.isDirectory()) {
      root_ = candidate;
      dir.close();
      break;
    }
    if (dir) dir.close();
  }

  if (root_.empty()) {
    LOG_DBG("DREG", "No dictionary directory found on SD card");
    return false;
  }

  auto root = Storage.open(root_.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  root.rewindDirectory();

  char name[500];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));

    if (!entry.isDirectory() || name[0] == '.') {
      entry.close();
      continue;
    }

    // Scan the subdirectory for .idx and .ifo files.
    // Folders with multiple .idx or multiple .ifo files are ambiguous and skipped.
    std::string subPath = root_ + "/" + name;
    entry.close();

    auto subDir = Storage.open(subPath.c_str());
    if (!subDir || !subDir.isDirectory()) {
      if (subDir) subDir.close();
      continue;
    }

    subDir.rewindDirectory();
    char subName[500];
    char foundStem[500];
    foundStem[0] = '\0';
    bool ambiguous = false;
    int ifoCount = 0;
    for (auto subEntry = subDir.openNextFile(); subEntry; subEntry = subDir.openNextFile()) {
      subEntry.getName(subName, sizeof(subName));

      // Skip macOS metadata files (AppleDouble resource forks, .DS_Store)
      if (strncmp(subName, "._", 2) == 0 || strcasecmp(subName, ".DS_Store") == 0) {
        subEntry.close();
        continue;
      }

      const size_t subLen = strlen(subName);
      const bool isIdx = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".idx") == 0;
      const bool isIfo = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".ifo") == 0;
      subEntry.close();

      if (isIfo) ifoCount++;
      if (isIdx) {
        if (foundStem[0] != '\0') {
          // Second .idx found — folder is ambiguous, skip it.
          ambiguous = true;
          LOG_DBG("DREG", "Skipping %s: multiple .idx files found", name);
          break;
        }
        subName[subLen - 4] = '\0';  // strip ".idx" to get stem
        strncpy(foundStem, subName, sizeof(foundStem) - 1);
      }
    }
    subDir.close();

    if (!ambiguous && ifoCount > 1) {
      ambiguous = true;
      LOG_DBG("DREG", "Skipping %s: multiple .ifo files found", name);
    }

    if (!ambiguous && foundStem[0] != '\0') {
      DictionaryEntry e;
      e.name = name;
      e.stem = foundStem;
      e.basePath = root_ + "/" + e.name + "/" + e.stem;
      entries_.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary: %s/%s", name, foundStem);
    }
  }

  root.close();

  // Sort alphabetically by folder name (case-insensitive — matches FileBrowserActivity).
  std::sort(entries_.begin(), entries_.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    const char* s1 = a.name.c_str();
    const char* s2 = b.name.c_str();
    while (*s1 && *s2) {
      char c1 = static_cast<char>(tolower(static_cast<unsigned char>(*s1)));
      char c2 = static_cast<char>(tolower(static_cast<unsigned char>(*s2)));
      if (c1 != c2) return c1 < c2;
      s1++;
      s2++;
    }
    return *s1 == '\0' && *s2 != '\0';
  });

  return !entries_.empty();
}

int DictionaryRegistry::indexOf(const std::string& basePath) const {
  if (basePath.empty()) return -1;
  for (size_t i = 0; i < entries_.size(); i++) {
    if (entries_[i].basePath == basePath) return static_cast<int>(i);
  }
  return -1;
}
