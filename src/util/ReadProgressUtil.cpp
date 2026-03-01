#include "ReadProgressUtil.h"

#include <HalStorage.h>

#include <functional>

#include "util/StringUtils.h"

namespace ReadProgressUtil {

bool hasBeenOpened(const std::string& filepath) {
  // Determine the cache prefix based on file extension
  const char* prefix = nullptr;
  if (StringUtils::checkFileExtension(filepath, ".epub")) {
    prefix = "epub_";
  } else if (StringUtils::checkFileExtension(filepath, ".txt") ||
             StringUtils::checkFileExtension(filepath, ".md")) {
    prefix = "txt_";
  } else if (StringUtils::checkFileExtension(filepath, ".xtc") ||
             StringUtils::checkFileExtension(filepath, ".xtch")) {
    prefix = "xtc_";
  }

  if (!prefix) {
    return false;  // Unsupported file type
  }

  // Compute cache path using the same hash as Epub/Txt/Xtc constructors
  const auto hash = std::hash<std::string>{}(filepath);
  const std::string progressPath =
      "/.crosspoint/" + std::string(prefix) + std::to_string(hash) + "/progress.bin";

  return Storage.exists(progressPath.c_str());
}

}  // namespace ReadProgressUtil
