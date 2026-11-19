#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>
#include <strings.h>

#include <cstring>

#include "ClippingStore.h"

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

void sleepImageChanged(const char* path) {
  while (path[0] == '/' && path[1] == '/') ++path;
  // FAT names are case-insensitive.
  if (strcasecmp(path, "/sleep.bmp") != 0 && strncasecmp(path, "/sleep/", 7) != 0 &&
      strncasecmp(path, "/.sleep/", 8) != 0) {
    return;
  }
  auto dir = Storage.open("/.crosspoint");
  if (!dir || !dir.isDirectory()) return;
  char cachePath[64] = "/.crosspoint/";
  constexpr size_t DIR_LEN = 13;
  char* const name = cachePath + DIR_LEN;
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    name[0] = '\0';
    entry.getName(name, sizeof(cachePath) - DIR_LEN);
    entry.close();
    // Only sleep_<key>.bmp: sleep_frame.bin must stay.
    const size_t len = strlen(name);
    if (len > 4 && strncmp(name, "sleep_", 6) == 0 && strcmp(name + len - 4, ".bmp") == 0) Storage.remove(cachePath);
  }
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  sleepImageChanged(path.c_str());
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}
