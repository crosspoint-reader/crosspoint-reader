#include "BookCacheUtils.h"

#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Xtc.h>

#include "ClippingStore.h"

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";
  constexpr char FB2_PREFIX[] = "fb2_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0 ||
         strncmp(name, FB2_PREFIX, std::size(FB2_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasFb2Extension(path)) {
    // FB2 caches in two places: the conversion dir (fb2_<hash>) holding the
    // synthetic EPUBs, and the Epub-side cache of each synthetic file
    // (epub_<hash(synthetic path)> with progress/sections). Clear both.
    const std::string dir = Fb2::cacheDirFor(path, "/.crosspoint");
    if (Storage.exists(dir.c_str())) {
      for (const String& name : Storage.listFiles(dir.c_str(), 64)) {
        if (!name.startsWith("book_") || !name.endsWith(".epub")) continue;
        Epub(dir + "/" + name.c_str(), "/.crosspoint").clearCache();
      }
      Storage.removeDir(dir.c_str());
    }
  } else if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}
