#include "BookMover.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <functional>
#include <vector>

#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr const char* CACHE_ROOT = "/.crosspoint";
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

namespace BookMover {

std::string cachePathFor(const std::string& path) {
  const char* prefix;
  if (FsHelpers::hasEpubExtension(path)) {
    prefix = "/epub_";
  } else if (FsHelpers::hasXtcExtension(path)) {
    prefix = "/xtc_";
  } else if (FsHelpers::hasTxtExtension(path)) {
    prefix = "/txt_";
  } else {
    return {};
  }
  // Must mirror the cache-key scheme of the Epub/Txt/Xtc constructors.
  return CACHE_ROOT + std::string(prefix) + std::to_string(std::hash<std::string>{}(path));
}

std::string buildDestination(const std::string& srcPath, const std::string& dstDir) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;
  const std::string dirPrefix = (dstDir == "/") ? "/" : dstDir + "/";

  std::string dstPath = dirPrefix + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = dirPrefix + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  if (Storage.exists(dstPath.c_str())) {
    // Exhausted the numbered-suffix range; fall back to a guaranteed-unique name.
    dstPath = dirPrefix + base + "_" + std::to_string(millis()) + ext;
  }
  return dstPath;
}

// Re-key the cache dir and repoint Recent Books / the resume pointer after a
// file has already been renamed from oldPath to newPath. No-op for file types
// without a cache; a failed cache rename is non-fatal (progress is lost).
static void migrateBookState(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = cachePathFor(oldPath);
  const std::string newCachePath = cachePathFor(newPath);
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMover", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
              newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
}

bool moveFile(const std::string& srcPath, const std::string& dstPath) {
  if (srcPath == dstPath) return true;

  LOG_INF("BookMover", "Moving %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("BookMover", "Failed to move file");
    return false;
  }

  migrateBookState(srcPath, dstPath);
  return true;
}

// Count files (not directories) in the subtree at rootPath. Same iterative
// walk as the migration pass; pre-computes the progress denominator and
// doubles as a pre-flight check that the whole tree is traversable. Returns
// false when any directory fails to open (count is then unreliable).
static bool countFilesInTree(const std::string& rootPath, char* nameBuffer, size_t& count) {
  count = 0;
  std::vector<std::string> stack;
  stack.reserve(8);
  stack.push_back(rootPath);

  while (!stack.empty()) {
    const std::string dirPath = std::move(stack.back());
    stack.pop_back();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      LOG_ERR("BookMover", "Cannot open dir: %s", dirPath.c_str());
      return false;
    }

    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuffer, NAME_BUFFER_SIZE);
      if (strcmp(nameBuffer, ".") == 0 || strcmp(nameBuffer, "..") == 0) {
        continue;
      }
      if (entry.isDirectory()) {
        std::string entryPath = dirPath;
        if (entryPath.back() != '/') entryPath += '/';
        entryPath += nameBuffer;
        entry.close();
        stack.push_back(std::move(entryPath));
      } else {
        entry.close();
        count++;
      }
    }
  }
  return true;
}

bool moveFolder(const std::string& srcPath, const std::string& dstPath, const ProgressCallback progressCb,
                void* progressCtx) {
  if (srcPath == dstPath) return true;
  if (srcPath.empty() || srcPath == "/" || dstPath.empty() || dstPath == "/") {
    LOG_ERR("BookMover", "Refusing to move to/from root");
    return false;
  }
  // Moving a folder into its own subtree would orphan it on the FAT chain.
  if (dstPath.rfind(srcPath + "/", 0) == 0) {
    LOG_ERR("BookMover", "Cannot move folder into itself: %s -> %s", srcPath.c_str(), dstPath.c_str());
    return false;
  }

  // Allocate the walk buffer and pre-validate the whole subtree BEFORE the
  // rename: once the rename commits, an unreadable directory or a missing
  // buffer would leave books inside with orphaned caches and no way back.
  // The per-book mappings themselves are not staged in RAM — they are a
  // deterministic prefix swap re-derived during the post-rename walk, and
  // staging them would cost O(files) heap. The pre-pass also provides the
  // true progress denominator (progress is counted in files; the folder move
  // itself is a single FAT rename).
  auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("BookMover", "OOM: name buffer; folder not moved");
    return false;
  }

  size_t totalFiles = 0;
  if (!countFilesInTree(srcPath, nameBuffer.get(), totalFiles)) {
    LOG_ERR("BookMover", "Subtree not fully readable; folder not moved");
    return false;
  }
  if (progressCb) {
    progressCb(0, totalFiles, progressCtx);
  }

  LOG_INF("BookMover", "Moving folder %s -> %s (%zu files)", srcPath.c_str(), dstPath.c_str(), totalFiles);
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("BookMover", "Failed to move folder");
    return false;
  }

  // The folder has moved, so every book inside now lives at a new path and its
  // path-hash-keyed cache must be re-keyed. Walk the moved subtree iteratively
  // (explicit stack, no recursion — task stacks are small). The subtree was
  // validated traversable just above, so failures here are unexpected.
  size_t doneFiles = 0;
  std::vector<std::string> stack;
  stack.reserve(8);
  stack.push_back(dstPath);

  while (!stack.empty()) {
    const std::string dirPath = std::move(stack.back());
    stack.pop_back();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      LOG_ERR("BookMover", "Failed to open dir during cache migration: %s", dirPath.c_str());
      continue;
    }

    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(nameBuffer.get(), ".") == 0 || strcmp(nameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = dirPath;
      if (entryPath.back() != '/') entryPath += '/';
      entryPath += nameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back(std::move(entryPath));
      } else {
        // Old path = same entry with the source folder prefix restored.
        const std::string oldEntryPath = srcPath + entryPath.substr(dstPath.size());
        migrateBookState(oldEntryPath, entryPath);
        doneFiles++;
        if (progressCb) {
          progressCb(doneFiles, totalFiles, progressCtx);
        }
      }
    }
  }
  return true;
}

}  // namespace BookMover
