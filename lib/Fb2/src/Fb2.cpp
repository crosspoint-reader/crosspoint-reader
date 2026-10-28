#include "Fb2.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <functional>

#include "Fb2Converter.h"

std::string Fb2::cacheDirFor(const std::string& fb2Path, const std::string& cacheRoot) {
  return cacheRoot + "/fb2_" + std::to_string(std::hash<std::string>{}(fb2Path));
}

std::string Fb2::ensureEpub(const std::string& fb2Path, const std::string& cacheRoot, std::string* error) {
  if (error) error->clear();

  HalFile f;
  if (!Storage.openFileForRead("FB2", fb2Path, f)) {
    if (error) *error = "cannot open book";
    return "";
  }
  const size_t srcSize = f.size();
  const uint32_t mtime = f.modificationTime();
  f.close();
  LOG_DBG("FB2", "ensureEpub: source size=%u mtime=%u", (unsigned)srcSize, (unsigned)mtime);

  const std::string dir = cacheDirFor(fb2Path, cacheRoot);
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    if (error) *error = "cannot create cache dir";
    LOG_ERR("FB2", "ensureEpub: cache dir failed: %s", dir.c_str());
    return "";
  }

  // The synthetic name carries the source fingerprint: a changed book gets a
  // new EPUB and with it a fresh Epub-side cache, without invalidation logic.
  char file[40];
  snprintf(file, sizeof(file), "book_%u_%u.epub", (unsigned)srcSize, (unsigned)mtime);
  const std::string target = dir + "/" + file;

  if (Storage.exists(target.c_str())) {
    LOG_DBG("FB2", "ensureEpub: cached epub exists: %s", target.c_str());
    return target;
  }

  // Drop conversions of older fingerprints and any leftover temp file.
  const std::vector<String> stale = Storage.listFiles(dir.c_str(), 256);
  LOG_DBG("FB2", "ensureEpub: %u stale files in cache dir", (unsigned)stale.size());
  for (const String& name : stale) {
    if (!name.startsWith("book_")) continue;
    if (name == file) continue;
    Storage.remove((dir + "/" + name.c_str()).c_str());
  }
  const std::string tmp = dir + "/book.tmp";
  Storage.remove(tmp.c_str());
  LOG_DBG("FB2", "ensureEpub: converting to %s", target.c_str());

  const uint32_t t0 = millis();
  std::string convertError;
  if (!Fb2Converter::convert(fb2Path, tmp, convertError)) {
    Storage.remove(tmp.c_str());
    if (error) *error = convertError.empty() ? "conversion failed" : convertError;
    LOG_ERR("FB2", "convert failed for %s: %s", fb2Path.c_str(), convertError.c_str());
    return "";
  }
  if (!Storage.replaceFile(tmp.c_str(), target.c_str())) {
    Storage.remove(tmp.c_str());
    if (error) *error = "cache write failed";
    LOG_ERR("FB2", "ensureEpub: replaceFile failed");
    return "";
  }
  LOG_INF("FB2", "synthetic epub ready in %lu ms: %s", millis() - t0, target.c_str());
  return target;
}
