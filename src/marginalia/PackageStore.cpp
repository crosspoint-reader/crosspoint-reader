#include "PackageStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

namespace Marginalia {

namespace {

bool hasRequiredString(JsonDocument& doc, const char* key) {
  return doc[key].is<const char*>() && doc[key].as<const char*>()[0] != '\0';
}

}  // namespace

void PackageStore::scan() {
  packages_.clear();
  hadScanError_ = false;

  FsFile root = Storage.open(PACKAGE_ROOT);
  if (!root) {
    LOG_DBG("MPKG", "Package root not found: %s", PACKAGE_ROOT);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("MPKG", "Package root is not a directory: %s", PACKAGE_ROOT);
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

    std::string packageDir = std::string(PACKAGE_ROOT) + "/" + nameBuffer;
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
  manifest.name = doc["name"].as<const char*>();
  manifest.version = doc["version"].as<const char*>();
  manifest.kind = doc["kind"].as<const char*>();
  manifest.execution = doc["execution"].as<const char*>();
  manifest.summary = doc["summary"] | "";
  manifest.author = doc["author"] | "";
  manifest.valid = true;
  return manifest;
}

}  // namespace Marginalia
