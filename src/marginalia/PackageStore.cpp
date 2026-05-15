#include "PackageStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

#include "PackageThemeHost.h"

namespace Marginalia {

namespace {

bool isAlphaNum(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool hasRequiredString(JsonDocument& doc, const char* key) {
  return doc[key].is<const char*>() && doc[key].as<const char*>()[0] != '\0';
}

bool isSafeSettingId(const std::string& value) {
  if (value.empty() || value.length() > 48 || !isAlphaNum(value[0])) {
    return false;
  }

  for (const char c : value) {
    if (isAlphaNum(c) || c == '.' || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

std::string packageStatePath(const std::string& packageId) {
  return std::string(PACKAGE_STATE_ROOT) + "/" + packageId + ".json";
}

bool loadPackageState(const std::string& packageId, JsonDocument& doc) {
  if (!isSafePackageId(packageId)) return false;

  const std::string path = packageStatePath(packageId);
  if (Storage.exists(path.c_str())) {
    const String json = Storage.readFile(path.c_str());
    const DeserializationError error = deserializeJson(doc, json);
    if (error) {
      LOG_ERR("MPKG", "Package state read failed for %s: %s", packageId.c_str(), error.c_str());
      doc.clear();
    }
  }

  doc["schemaVersion"] = 1;
  doc["id"] = packageId;
  if (!doc["enabled"].is<bool>()) {
    doc["enabled"] = true;
  }
  if (!doc["settings"].is<JsonObject>()) {
    doc["settings"].to<JsonObject>();
  }
  return true;
}

bool savePackageState(const std::string& packageId, JsonDocument& doc) {
  if (!isSafePackageId(packageId) || !ensurePackageBaseDirectories()) return false;

  const std::string packagePath = std::string(PACKAGE_ROOT) + "/" + packageId;
  if (!Storage.exists(packagePath.c_str())) return false;

  doc["schemaVersion"] = 1;
  doc["id"] = packageId;

  String json;
  serializeJson(doc, json);
  const bool saved = Storage.writeFile(packageStatePath(packageId).c_str(), json);
  if (saved) {
    markPackageThemeHostDirty();
  }
  return saved;
}

bool arrayContains(JsonVariantConst value, const char* expected) {
  if (!value.is<JsonArrayConst>()) return false;
  for (JsonVariantConst item : value.as<JsonArrayConst>()) {
    if (item.is<const char*>() && std::string(item.as<const char*>()) == expected) {
      return true;
    }
  }
  return false;
}

bool arrayContainsAny(JsonVariantConst value, const char* first, const char* second) {
  return arrayContains(value, first) || arrayContains(value, second);
}

std::vector<PackageSettingDefinition> parsePackageSettings(JsonDocument& doc) {
  std::vector<PackageSettingDefinition> settings;
  JsonVariant settingsValue = doc["settings"];
  if (!settingsValue.is<JsonArray>()) return settings;

  for (JsonVariant settingValue : settingsValue.as<JsonArray>()) {
    if (!settingValue.is<JsonObject>()) continue;
    JsonObject settingObject = settingValue.as<JsonObject>();
    if (!settingObject["id"].is<const char*>() || !settingObject["label"].is<const char*>() ||
        !settingObject["type"].is<const char*>()) {
      continue;
    }

    PackageSettingDefinition setting;
    setting.id = settingObject["id"].as<const char*>();
    setting.label = settingObject["label"].as<const char*>();
    const std::string type = settingObject["type"].as<const char*>();
    if (!isSafeSettingId(setting.id) || setting.label.empty()) continue;

    if (type == "boolean") {
      setting.type = PackageSettingType::Boolean;
      setting.defaultBool = settingObject["default"] | false;
      settings.push_back(std::move(setting));
      continue;
    }

    if (type == "enum") {
      setting.type = PackageSettingType::Enum;
      JsonVariant options = settingObject["options"];
      if (!options.is<JsonArray>()) continue;
      for (JsonVariant option : options.as<JsonArray>()) {
        if (option.is<const char*>()) {
          setting.options.push_back(option.as<const char*>());
        }
      }
      if (setting.options.empty()) continue;
      setting.defaultString =
          settingObject["default"].is<const char*>() ? settingObject["default"].as<const char*>() : setting.options[0];
      if (std::find(setting.options.begin(), setting.options.end(), setting.defaultString) == setting.options.end()) {
        setting.defaultString = setting.options[0];
      }
      settings.push_back(std::move(setting));
    }
  }

  return settings;
}

bool parseVersionTriplet(const char* value, int (&parts)[3]) {
  if (value == nullptr || value[0] == '\0') return false;

  const char* cursor = value;
  for (int i = 0; i < 3; i++) {
    char* end = nullptr;
    const long parsed = strtol(cursor, &end, 10);
    if (end == cursor || parsed < 0 || parsed > 9999) {
      return false;
    }
    parts[i] = static_cast<int>(parsed);
    if (i < 2) {
      if (*end != '.') return false;
      cursor = end + 1;
    }
  }
  return true;
}

bool firmwareMeetsMinimum(const char* minimum) {
  int current[3] = {0, 0, 0};
  int required[3] = {0, 0, 0};
  if (!parseVersionTriplet(CROSSPOINT_VERSION, current) || !parseVersionTriplet(minimum, required)) {
    return false;
  }

  for (int i = 0; i < 3; i++) {
    if (current[i] > required[i]) return true;
    if (current[i] < required[i]) return false;
  }
  return true;
}

std::string compatibilityError(JsonDocument& doc) {
  JsonVariantConst target = doc["target"];
  if (!target.is<JsonObjectConst>()) return "";

  JsonVariantConst devices = target["devices"];
  if (devices.is<JsonArrayConst>() && !arrayContainsAny(devices, "xteink-x3", "xteink-x4")) {
    return "unsupported device";
  }

  JsonVariantConst chipFamilies = target["chipFamilies"];
  if (chipFamilies.is<JsonArrayConst>() && !arrayContains(chipFamilies, "esp32-c3")) {
    return "unsupported chip family";
  }

  const int apiLevel = target["apiLevel"] | 1;
  if (apiLevel > PACKAGE_API_LEVEL) {
    return "requires newer package API";
  }

  if ((target["requiresPSRAM"] | false) == true) {
    return "requires PSRAM";
  }

  if (target["minFirmware"].is<const char*>() && !firmwareMeetsMinimum(target["minFirmware"].as<const char*>())) {
    return "requires newer firmware";
  }

  return "";
}

}  // namespace

void PackageStore::scan() { scanRoot(PACKAGE_ROOT); }

void PackageStore::scanInbox() { scanRoot(PACKAGE_INBOX_ROOT); }

void PackageStore::scanRoot(const char* rootPath) {
  packages_.clear();
  hadScanError_ = false;

  FsFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("MPKG", "Package root not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("MPKG", "Package root is not a directory: %s", rootPath);
    hadScanError_ = true;
    return;
  }

  char nameBuffer[128];
  while (true) {
    FsFile entry = root.openNextFile();
    if (!entry) break;

    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    std::string packageDir = std::string(rootPath) + "/" + nameBuffer;
    PackageManifest manifest = readManifest(packageDir, nameBuffer);
    if (!manifest.valid) {
      hadScanError_ = true;
      LOG_ERR("MPKG", "Skipping package %s: %s", nameBuffer, manifest.error.c_str());
      continue;
    }

    packages_.push_back(std::move(manifest));
  }
}

PackageManifest PackageStore::readManifest(const std::string& packageDir, const std::string& packageDirName) const {
  PackageManifest manifest;
  manifest.id = packageDirName;
  manifest.directoryName = packageDirName;
  manifest.manifestPath = packageDir + "/manifest.json";

  FsFile file;
  if (!Storage.openFileForRead("MPKG", manifest.manifestPath.c_str(), file)) {
    manifest.error = "manifest.json missing";
    return manifest;
  }

  const auto manifestSize = file.size();
  file.close();
  if (manifestSize == 0) {
    manifest.error = "manifest.json is empty";
    return manifest;
  }
  if (manifestSize > MAX_MANIFEST_BYTES) {
    manifest.error = "manifest.json is too large";
    return manifest;
  }

  String json = Storage.readFile(manifest.manifestPath.c_str());
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    manifest.error = error.c_str();
    return manifest;
  }

  if ((doc["schemaVersion"] | 0) != 1) {
    manifest.error = "unsupported schema version";
    return manifest;
  }

  if (!hasRequiredString(doc, "id") || !hasRequiredString(doc, "name") || !hasRequiredString(doc, "version") ||
      !hasRequiredString(doc, "kind") || !hasRequiredString(doc, "execution")) {
    manifest.error = "required fields missing";
    return manifest;
  }

  manifest.id = doc["id"].as<const char*>();
  if (!isSafePackageId(manifest.id)) {
    manifest.error = "invalid package id";
    return manifest;
  }

  manifest.name = doc["name"].as<const char*>();
  manifest.version = doc["version"].as<const char*>();
  manifest.kind = doc["kind"].as<const char*>();
  manifest.execution = doc["execution"].as<const char*>();
  manifest.summary = doc["summary"] | "";
  manifest.author = doc["author"] | "";
  JsonArray permissions = doc["permissions"].as<JsonArray>();
  for (JsonVariant permission : permissions) {
    if (permission.is<const char*>()) {
      manifest.permissions.push_back(permission.as<const char*>());
    }
  }
  manifest.settings = parsePackageSettings(doc);
  manifest.enabled = readPackageEnabled(manifest.id);
  manifest.compatibilityError = compatibilityError(doc);
  manifest.compatible = manifest.compatibilityError.empty();
  manifest.valid = true;
  return manifest;
}

bool isSafePackageId(const std::string& value) {
  if (value.length() < 2 || value.length() > 96 || !isAlphaNum(value[0])) {
    return false;
  }

  for (const char c : value) {
    if (isAlphaNum(c) || c == '.' || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool isSafePackageRelativePath(const std::string& value) {
  if (value.empty() || value.length() > 180 || value[0] == '/' || value.find('\\') != std::string::npos ||
      value.find("//") != std::string::npos) {
    return false;
  }

  std::string component;
  for (size_t i = 0; i <= value.length(); i++) {
    const char c = (i < value.length()) ? value[i] : '/';
    if (c == '/') {
      if (component.empty() || component == "." || component == ".." || component[0] == '.') {
        return false;
      }
      component.clear();
      continue;
    }

    const bool safeChar = isAlphaNum(c) || c == '.' || c == '_' || c == '-';
    if (!safeChar) return false;
    component += c;
  }
  return true;
}

bool ensurePackageBaseDirectories() {
  return Storage.ensureDirectoryExists("/.marginalia") && Storage.ensureDirectoryExists(PACKAGE_ROOT) &&
         Storage.ensureDirectoryExists(PACKAGE_INBOX_ROOT) && Storage.ensureDirectoryExists(PACKAGE_STAGING_ROOT) &&
         Storage.ensureDirectoryExists(PACKAGE_STATE_ROOT);
}

bool readPackageEnabled(const std::string& packageId) {
  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return false;
  return doc["enabled"] | true;
}

bool setPackageEnabled(const std::string& packageId, const bool enabled) {
  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return false;
  doc["enabled"] = enabled;
  return savePackageState(packageId, doc);
}

bool readPackageSettingBool(const std::string& packageId, const std::string& settingId, const bool defaultValue) {
  if (!isSafeSettingId(settingId)) return defaultValue;

  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return defaultValue;
  JsonVariant value = doc["settings"][settingId.c_str()];
  return value.is<bool>() ? value.as<bool>() : defaultValue;
}

std::string readPackageSettingString(const std::string& packageId, const std::string& settingId,
                                     const std::string& defaultValue) {
  if (!isSafeSettingId(settingId)) return defaultValue;

  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return defaultValue;
  JsonVariant value = doc["settings"][settingId.c_str()];
  return value.is<const char*>() ? value.as<const char*>() : defaultValue;
}

bool writePackageSettingBool(const std::string& packageId, const std::string& settingId, const bool value) {
  if (!isSafeSettingId(settingId)) return false;

  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return false;
  doc["settings"][settingId.c_str()] = value;
  return savePackageState(packageId, doc);
}

bool writePackageSettingString(const std::string& packageId, const std::string& settingId, const std::string& value) {
  if (!isSafeSettingId(settingId)) return false;

  JsonDocument doc;
  if (!loadPackageState(packageId, doc)) return false;
  doc["settings"][settingId.c_str()] = value;
  return savePackageState(packageId, doc);
}

bool uninstallPackage(const std::string& packageId) {
  if (!isSafePackageId(packageId) || !ensurePackageBaseDirectories()) {
    return false;
  }

  const std::string packagePath = std::string(PACKAGE_ROOT) + "/" + packageId;
  if (!Storage.exists(packagePath.c_str())) {
    return false;
  }

  if (!Storage.removeDir(packagePath.c_str())) {
    return false;
  }

  const std::string statePath = packageStatePath(packageId);
  if (Storage.exists(statePath.c_str())) {
    Storage.remove(statePath.c_str());
  }
  markPackageThemeHostDirty();
  return true;
}

}  // namespace Marginalia
