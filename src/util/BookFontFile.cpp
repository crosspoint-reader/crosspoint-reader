#include "BookFontFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

namespace {
std::string fontPath(const std::string& cacheDir) { return cacheDir + "/font.json"; }
}  // namespace

bool BookFontFile::load(const std::string& cacheDir, ReaderFont& font) {
  const std::string path = fontPath(cacheDir);
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;
  font.setFamily(doc["sdFontFamilyName"] | "");
  font.fontPointSize = doc["fontSize"] | static_cast<uint8_t>(0);
  return font.fontPointSize != 0;
}

bool BookFontFile::save(const std::string& cacheDir, const ReaderFont& font) {
  JsonDocument doc;
  doc["sdFontFamilyName"] = font.sdFontFamilyName;
  doc["fontSize"] = font.fontPointSize;
  const std::string path = fontPath(cacheDir);
  if (!PersistableStoreBase::writeDocToFile(path.c_str(), doc)) {
    LOG_ERR("BFNT", "Failed to save book font");
    return false;
  }
  return true;
}

void BookFontFile::remove(const std::string& cacheDir) {
  const std::string path = fontPath(cacheDir);
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}
