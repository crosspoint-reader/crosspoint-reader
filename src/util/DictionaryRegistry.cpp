#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "StringUtils.h"

namespace DictionaryRegistry {
namespace {

constexpr const char* DICT_ROOT = "/dictionaries";

// Find the single .idx stem inside one dictionary folder. Returns false when
// the folder holds no .idx or more than one distinct stem (ambiguous).
bool findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return false;

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_DBG("DREG", "Skipping %s: multiple index stems found", folderPath);
      return false;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  if (foundStem[0] == '\0') return false;

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", folderPath);
    return false;
  }

  stemOut = foundStem;
  return true;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);

  auto rootDir = Storage.open(DICT_ROOT);
  if (!rootDir || !rootDir.isDirectory()) {
    LOG_DBG("DREG", "No %s directory on SD card", DICT_ROOT);
    return;
  }

  rootDir.rewindDirectory();
  char name[128];
  for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (!entry.isDirectory() || name[0] == '.') continue;

    std::string folderPath = std::string(DICT_ROOT) + "/" + name;
    std::string stem;
    if (!findStem(folderPath.c_str(), stem)) continue;

    DictionaryEntry e;
    e.name = name;
    e.stem = std::move(stem);
    out.push_back(std::move(e));
    LOG_DBG("DREG", "Found dictionary: %s", name);
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape /dictionaries/.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;
  std::string folderPath = std::string(DICT_ROOT) + "/" + folderName;
  std::string stem;
  if (!findStem(folderPath.c_str(), stem)) return false;
  basePathOut = folderPath + "/" + stem;
  return true;
}

}  // namespace DictionaryRegistry
