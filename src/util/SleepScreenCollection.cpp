#include "SleepScreenCollection.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace SleepScreenCollection {
namespace {

constexpr size_t NAME_BUFFER_SIZE = 1024;
constexpr size_t EXPECTED_SET_COUNT = 8;

struct ScanScratch {
  std::optional<Bitmap> bitmap;
  char name[NAME_BUFFER_SIZE];
  char setName[MAX_SET_NAME_BYTES + 1];
};

bool shouldSkipEntry(const char* name) {
  return !name || name[0] == '\0' || name[0] == '.' || strcmp(name, "System Volume Information") == 0;
}

bool isSafeSelectedSetName(const char* name) {
  if (shouldSkipEntry(name)) return false;
  const size_t length = strlen(name);
  return length <= MAX_SET_NAME_BYTES && strpbrk(name, "/\\") == nullptr;
}

std::string joinPath(const std::string& directory, const char* name) {
  std::string path;
  path.reserve(directory.size() + 1 + strlen(name));
  path = directory;
  if (path.empty() || path.back() != '/') path += '/';
  path += name;
  return path;
}

bool isValidBitmap(HalFile& file, const char* name, ScanScratch& scratch) {
  if (!FsHelpers::hasBmpExtension(name)) {
    LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
    return false;
  }

  // Bitmap is too large for the task stack, so one heap slot is reused for all
  // header checks in a scan.
  scratch.bitmap.emplace(file);
  const BmpReaderError result = scratch.bitmap->parseHeaders();
  scratch.bitmap.reset();
  if (result != BmpReaderError::Ok) LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
  return result == BmpReaderError::Ok;
}

bool scanDirectory(HalFile& directory, ScanScratch& scratch) {
  if (!directory || !directory.isDirectory()) return false;

  while (auto entry = directory.openNextFile()) {
    if (entry.getName(scratch.name, sizeof(scratch.name)) == 0 || entry.isDirectory() ||
        shouldSkipEntry(scratch.name) || !isValidBitmap(entry, scratch.name, scratch)) {
      continue;
    }
    return true;
  }
  return false;
}

}  // namespace

const char* resolveDirectory() {
  auto hidden = Storage.open("/.sleep");
  if (hidden && hidden.isDirectory()) return "/.sleep";
  auto visible = Storage.open("/sleep");
  return visible && visible.isDirectory() ? "/sleep" : nullptr;
}

void discover(const char* sleepDirectory, std::vector<std::string>& sets) {
  sets.clear();
  sets.reserve(EXPECTED_SET_COUNT);
  if (!sleepDirectory) return;

  auto scratch = makeUniqueNoThrow<ScanScratch>();
  if (!scratch) {
    LOG_ERR("SLP", "OOM: sleep-screen scan scratch");
    return;
  }
  auto root = Storage.open(sleepDirectory);
  if (!root || !root.isDirectory()) return;

  bool hasDefault = false;
  while (auto entry = root.openNextFile()) {
    const size_t nameLength = entry.getName(scratch->name, sizeof(scratch->name));
    if (nameLength == 0 || shouldSkipEntry(scratch->name)) continue;
    if (!entry.isDirectory()) {
      if (isValidBitmap(entry, scratch->name, *scratch)) hasDefault = true;
    } else if (nameLength <= MAX_SET_NAME_BYTES) {
      memcpy(scratch->setName, scratch->name, nameLength + 1);
      if (scanDirectory(entry, *scratch)) sets.emplace_back(scratch->setName);
    }
  }

  std::sort(sets.begin(), sets.end(), FsHelpers::naturalLess);
  if (hasDefault) sets.insert(sets.begin(), "");
}

bool resolveSelectedDirectory(const char* sleepDirectory, const char* selectedSet, std::string& selectedDirectory) {
  selectedDirectory = sleepDirectory ? sleepDirectory : "";
  if (!sleepDirectory || !selectedSet || selectedSet[0] == '\0') return false;
  if (!isSafeSelectedSetName(selectedSet)) {
    LOG_ERR("SLP", "Ignoring invalid persisted sleep-screen set name");
    return false;
  }

  const std::string requestedPath = joinPath(sleepDirectory, selectedSet);
  auto requested = Storage.open(requestedPath.c_str());
  if (!requested || !requested.isDirectory()) return false;
  selectedDirectory = requestedPath;
  return true;
}

}  // namespace SleepScreenCollection
