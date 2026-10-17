#include "FileReplace.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace FileReplace {
namespace {

// The handle closes on return, before anything is renamed.
bool isFolder(const char* path) {
  HalFile file = Storage.open(path);
  return file && file.isDirectory();
}

}  // namespace

bool replace(const char* fromPath, const char* destPath) {
  if (!Storage.exists(destPath)) return Storage.rename(fromPath, destPath);
  if (isFolder(destPath)) {
    LOG_ERR("FRP", "Not replacing %s: it is a folder", destPath);
    return false;
  }

  const std::string aside = std::string(destPath) + kAsideSuffix;
  if (Storage.exists(aside.c_str())) {
    LOG_ERR("FRP", "Not replacing %s: %s is in the way", destPath, aside.c_str());
    return false;
  }
  if (!Storage.rename(destPath, aside.c_str())) {
    LOG_ERR("FRP", "Not replacing %s: it could not be set aside", destPath);
    return false;
  }
  if (!Storage.rename(fromPath, destPath)) {
    if (Storage.rename(aside.c_str(), destPath)) {
      LOG_ERR("FRP", "Could not move %s to %s; the old file is back", fromPath, destPath);
    } else {
      LOG_ERR("FRP", "Could not move %s to %s; the old file stays as %s", fromPath, destPath, aside.c_str());
    }
    return false;
  }
  if (!Storage.remove(aside.c_str())) {
    LOG_ERR("FRP", "Replaced %s; the old file stays as %s", destPath, aside.c_str());
  }
  return true;
}

}  // namespace FileReplace
