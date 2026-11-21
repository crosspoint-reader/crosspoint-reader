#pragma once
#include <ArduinoJson.h>
#include <HalStorage.h>

#include <string>

struct PersistableStoreBase {
  static bool writeDocToFile(const char* path, const JsonDocument& doc) {
    std::string json;
    serializeJson(doc, json);
    fake::add(path, json);
    return true;
  }
  static bool readDocFromFile(const char* path, JsonDocument& doc) {
    const auto found = fake::files.find(path);
    if (found == fake::files.end()) return false;
    return !deserializeJson(doc, std::string(found->second->bytes.begin(), found->second->bytes.end()));
  }
};
