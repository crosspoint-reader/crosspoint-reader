#include "BookMover.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <functional>

#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr const char* CACHE_ROOT = "/.crosspoint";
}

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

bool moveFile(const std::string& srcPath, const std::string& dstPath) {
  if (srcPath == dstPath) return true;

  LOG_INF("BookMover", "Moving %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("BookMover", "Failed to move file");
    return false;
  }

  // Re-key the cache dir so reading progress survives the move.
  const std::string oldCachePath = cachePathFor(srcPath);
  const std::string newCachePath = cachePathFor(dstPath);
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMover", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
              newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
  return true;
}

}  // namespace BookMover
