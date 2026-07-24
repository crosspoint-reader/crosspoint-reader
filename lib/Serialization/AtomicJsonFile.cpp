#include "AtomicJsonFile.h"

#include <ArduinoJson.h>

namespace AtomicJsonFile {
namespace {

bool validateJson(const uint8_t* data, const size_t size, void*) {
  JsonDocument doc;
  return !deserializeJson(doc, data, size);
}

}  // namespace

AtomicFile::LoadStatus load(const char* path, std::string& json, const size_t maxSize) {
  return AtomicFile::load(path, json, maxSize, validateJson);
}

AtomicFile::SaveStatus save(const char* path, const char* json, const size_t size, const size_t maxSize) {
  return AtomicFile::save(path, reinterpret_cast<const uint8_t*>(json), size, maxSize, validateJson);
}

}  // namespace AtomicJsonFile
