#include "AppLoader.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <memory>

#include "Battery.h"

namespace CrossPoint {

namespace {
bool renameFileAtomic(const String& from, const String& to) {
  if (!SdMan.exists(from.c_str())) {
    return false;
  }
  if (SdMan.exists(to.c_str())) {
    SdMan.remove(to.c_str());
  }

  FsFile src = SdMan.open(from.c_str(), O_RDONLY);
  if (!src) {
    return false;
  }

  // Try SdFat rename first.
  if (src.rename(to.c_str())) {
    src.close();
    return true;
  }
  src.close();

  // Fallback: copy + delete.
  FsFile in = SdMan.open(from.c_str(), O_RDONLY);
  if (!in) {
    return false;
  }

  FsFile out;
  if (!SdMan.openFileForWrite("AppLoader", to, out)) {
    in.close();
    return false;
  }

  static uint8_t copyBuf[2048];
  while (true) {
    const int n = in.read(copyBuf, sizeof(copyBuf));
    if (n <= 0) {
      break;
    }
    if (out.write(copyBuf, n) != static_cast<size_t>(n)) {
      out.close();
      in.close();
      SdMan.remove(to.c_str());
      return false;
    }
  }

  out.close();
  in.close();
  SdMan.remove(from.c_str());
  return true;
}
}  // namespace

std::vector<AppInfo> AppLoader::scanApps() {
  std::vector<AppInfo> apps;

  if (!isSDReady()) {
    Serial.printf("[%lu] [AppLoader] SD card not ready\n", millis());
    return apps;
  }

  FsFile appsDir = SdMan.open(APPS_BASE_PATH, O_RDONLY);
  if (!appsDir || !appsDir.isDirectory()) {
    Serial.printf("[%lu] [AppLoader] Apps directory not found: %s\n", millis(), APPS_BASE_PATH);
    if (appsDir) appsDir.close();
    return apps;
  }

  char name[128];
  FsFile entry = appsDir.openNextFile();

  while (entry) {
    if (entry.isDirectory()) {
      entry.getName(name, sizeof(name));

      String appPath = APPS_BASE_PATH;
      if (!appPath.endsWith("/")) {
        appPath += "/";
      }
      appPath += name;

      String manifestPath = buildManifestPath(appPath);
      AppManifest manifest = parseManifest(manifestPath);

      if (!manifest.name.isEmpty()) {
        apps.emplace_back(manifest, appPath);
        Serial.printf("[%lu] [AppLoader] Found app: %s\n", millis(), manifest.name.c_str());
      } else {
        Serial.printf("[%lu] [AppLoader] Skipping directory (no valid manifest): %s\n", millis(), name);
      }
    }

    entry.close();
    entry = appsDir.openNextFile();
  }

  appsDir.close();

  Serial.printf("[%lu] [AppLoader] Found %u app(s)\n", millis(), apps.size());
  return apps;
}

AppManifest AppLoader::parseManifest(const String& path) {
  AppManifest manifest;

  if (!isSDReady()) {
    Serial.printf("[%lu] [AppLoader] SD card not ready, cannot parse manifest\n", millis());
    return manifest;
  }

  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [AppLoader] Manifest file not found: %s\n", millis(), path.c_str());
    return manifest;
  }

  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) {
    Serial.printf("[%lu] [AppLoader] Failed to open manifest file: %s\n", millis(), path.c_str());
    return manifest;
  }

  const size_t fileSize = file.size();
  if (fileSize == 0) {
    Serial.printf("[%lu] [AppLoader] Manifest file is empty: %s\n", millis(), path.c_str());
    file.close();
    return manifest;
  }

  if (fileSize > MAX_MANIFEST_SIZE) {
    Serial.printf("[%lu] [AppLoader] Manifest file too large (%u bytes, max %u): %s\n", millis(), fileSize,
                  MAX_MANIFEST_SIZE, path.c_str());
    file.close();
    return manifest;
  }

  std::unique_ptr<char[]> buffer(new char[fileSize + 1]);
  const size_t bytesRead = file.read(buffer.get(), fileSize);
  buffer[bytesRead] = '\0';
  file.close();

  if (bytesRead != fileSize) {
    Serial.printf("[%lu] [AppLoader] Failed to read complete manifest file (read %u of %u bytes): %s\n", millis(),
                  bytesRead, fileSize, path.c_str());
    return manifest;
  }

  // Handle UTF-8 BOM if the manifest was created by an editor that writes it.
  const char* json = buffer.get();
  if (bytesRead >= 3 && static_cast<uint8_t>(json[0]) == 0xEF && static_cast<uint8_t>(json[1]) == 0xBB &&
      static_cast<uint8_t>(json[2]) == 0xBF) {
    json += 3;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.printf("[%lu] [AppLoader] JSON parse error in %s: %s\n", millis(), path.c_str(), error.c_str());
    return manifest;
  }

  if (doc["name"].is<const char*>()) {
    manifest.name = doc["name"].as<String>();
  } else {
    Serial.printf("[%lu] [AppLoader] Missing or invalid 'name' field in: %s\n", millis(), path.c_str());
    return manifest;
  }

  if (doc["version"].is<const char*>()) {
    manifest.version = doc["version"].as<String>();
  } else {
    manifest.version = "1.0.0";
  }

  if (doc["description"].is<const char*>()) {
    manifest.description = doc["description"].as<String>();
  } else {
    manifest.description = "";
  }

  if (doc["author"].is<const char*>()) {
    manifest.author = doc["author"].as<String>();
  } else {
    manifest.author = "Unknown";
  }

  if (doc["minFirmware"].is<const char*>()) {
    manifest.minFirmware = doc["minFirmware"].as<String>();
  } else {
    manifest.minFirmware = "0.0.0";
  }

  return manifest;
}

bool AppLoader::flashApp(const String& binPath, ProgressCallback callback) {
  if (!isSDReady()) {
    Serial.printf("[%lu] [AppLoader] SD card not ready, cannot flash app\n", millis());
    return false;
  }

  const uint16_t batteryPercentage = battery.readPercentage();
  if (batteryPercentage < 20) {
    Serial.printf("[%lu] [AppLoader] Battery: %u%% - TOO LOW\n", millis(), batteryPercentage);
    Serial.printf("[%lu] [AppLoader] Flash aborted: battery below 20%%\n", millis());
    return false;
  }
  Serial.printf("[%lu] [AppLoader] Battery: %u%% - OK\n", millis(), batteryPercentage);

  if (!SdMan.exists(binPath.c_str())) {
    Serial.printf("[%lu] [AppLoader] App binary not found: %s\n", millis(), binPath.c_str());
    return false;
  }

  FsFile file = SdMan.open(binPath.c_str(), O_RDONLY);
  if (!file) {
    Serial.printf("[%lu] [AppLoader] Failed to open app binary: %s\n", millis(), binPath.c_str());
    return false;
  }

  const size_t fileSize = file.size();
  if (fileSize == 0) {
    Serial.printf("[%lu] [AppLoader] App binary is empty: %s\n", millis(), binPath.c_str());
    file.close();
    return false;
  }

  uint8_t magicByte = 0;
  const size_t magicRead = file.read(&magicByte, 1);
  if (magicRead != 1 || magicByte != 0xE9) {
    Serial.printf("[%lu] [AppLoader] Invalid firmware magic byte: 0x%02X\n", millis(), magicByte);
    file.close();
    return false;
  }

  file.close();
  file = SdMan.open(binPath.c_str(), O_RDONLY);
  if (!file) {
    Serial.printf("[%lu] [AppLoader] Failed to reopen app binary: %s\n", millis(), binPath.c_str());
    return false;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) {
    Serial.printf("[%lu] [AppLoader] Failed to get running partition\n", millis());
    file.close();
    return false;
  }

  const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
  if (!target) {
    Serial.printf("[%lu] [AppLoader] No OTA partition available\n", millis());
    file.close();
    return false;
  }

  if (target->address == running->address) {
    Serial.printf("[%lu] [AppLoader] Target partition matches running partition, aborting\n", millis());
    file.close();
    return false;
  }

  if (fileSize >= target->size) {
    Serial.printf("[%lu] [AppLoader] Firmware too large (%u bytes, max %u)\n", millis(), fileSize, target->size);
    file.close();
    return false;
  }

  Serial.printf("[%lu] [AppLoader] Flashing to partition: %s (offset: 0x%06X)\n", millis(), target->label,
                target->address);

  esp_ota_handle_t otaHandle = 0;
  esp_err_t err = esp_ota_begin(target, fileSize, &otaHandle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [AppLoader] OTA begin failed: %d\n", millis(), err);
    file.close();
    return false;
  }

  if (callback) {
    callback(0, fileSize);
  }

  size_t totalWritten = 0;
  // Larger chunks reduce SD/OTA overhead significantly.
  // 32KB is a good balance on ESP32-C3: faster writes without blowing RAM.
  static constexpr size_t flashChunkSize = 32 * 1024;
  static uint8_t buffer[flashChunkSize];

  size_t lastNotifiedPercent = 0;

  while (totalWritten < fileSize) {
    const size_t remaining = fileSize - totalWritten;
    const size_t toRead = remaining < flashChunkSize ? remaining : flashChunkSize;
    const size_t bytesRead = file.read(buffer, toRead);
    if (bytesRead == 0) {
      Serial.printf("[%lu] [AppLoader] Failed to read firmware data\n", millis());
      esp_ota_end(otaHandle);
      file.close();
      return false;
    }

    err = esp_ota_write(otaHandle, buffer, bytesRead);
    if (err != ESP_OK) {
      Serial.printf("[%lu] [AppLoader] OTA write failed at %u/%u bytes: %d\n", millis(), totalWritten, fileSize, err);
      esp_ota_end(otaHandle);
      file.close();
      return false;
    }

    totalWritten += bytesRead;

    if (callback) {
      const size_t percent = (totalWritten * 100) / fileSize;
      // Throttle UI updates; each screen refresh is ~400ms.
      if (percent >= lastNotifiedPercent + 10 || percent == 100) {
        lastNotifiedPercent = percent;
        callback(totalWritten, fileSize);
      }
    }
  }

  file.close();

  err = esp_ota_end(otaHandle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [AppLoader] OTA end failed: %d\n", millis(), err);
    return false;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [AppLoader] Failed to set boot partition: %d\n", millis(), err);
    return false;
  }

  Serial.printf("[%lu] [AppLoader] Flash complete. Boot partition set: %s\n", millis(), target->label);
  return true;
}

String AppLoader::calculateFileSHA256(const String& path) {
  if (!isSDReady()) {
    Serial.printf("[%lu] [AppLoader] SD card not ready, cannot hash file\n", millis());
    return "";
  }

  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [AppLoader] File not found for hashing: %s\n", millis(), path.c_str());
    return "";
  }

  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) {
    Serial.printf("[%lu] [AppLoader] Failed to open file for hashing: %s\n", millis(), path.c_str());
    return "";
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    file.close();
    Serial.printf("[%lu] [AppLoader] Failed to init SHA256\n", millis());
    return "";
  }

  static uint8_t buf[4096];
  while (true) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    if (mbedtls_sha256_update_ret(&ctx, buf, static_cast<size_t>(n)) != 0) {
      mbedtls_sha256_free(&ctx);
      file.close();
      Serial.printf("[%lu] [AppLoader] Failed updating SHA256\n", millis());
      return "";
    }
  }

  file.close();

  uint8_t hash[32];
  if (mbedtls_sha256_finish_ret(&ctx, hash) != 0) {
    mbedtls_sha256_free(&ctx);
    Serial.printf("[%lu] [AppLoader] Failed finishing SHA256\n", millis());
    return "";
  }
  mbedtls_sha256_free(&ctx);

  String out;
  out.reserve(64);
  for (size_t i = 0; i < sizeof(hash); i++) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", hash[i]);
    out += hex;
  }
  return out;
}

bool AppLoader::loadInstalledState(JsonDocument& doc) {
  doc.clear();
  doc.to<JsonObject>();

  if (!isSDReady()) {
    return false;
  }

  if (!SdMan.exists(INSTALLED_STATE_PATH)) {
    return true;
  }

  FsFile file = SdMan.open(INSTALLED_STATE_PATH, O_RDONLY);
  if (!file) {
    return false;
  }

  const DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.printf("[%lu] [AppLoader] Installed state JSON parse error: %s\n", millis(), err.c_str());
    doc.clear();
    doc.to<JsonObject>();
  }

  return true;
}

bool AppLoader::saveInstalledState(const JsonDocument& doc) {
  if (!isSDReady()) {
    return false;
  }

  if (!SdMan.ensureDirectoryExists("/.crosspoint") || !SdMan.ensureDirectoryExists(APPS_BASE_PATH)) {
    return false;
  }

  String json;
  serializeJson(doc, json);

  const String tmpPath = String(INSTALLED_STATE_PATH) + ".tmp";
  if (!SdMan.writeFile(tmpPath.c_str(), json)) {
    return false;
  }

  if (!renameFileAtomic(tmpPath, INSTALLED_STATE_PATH)) {
    SdMan.remove(tmpPath.c_str());
    return false;
  }

  return true;
}

bool AppLoader::isAppInstalled(const String& appId, const String& sha256) {
  if (appId.isEmpty() || sha256.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  (void)loadInstalledState(doc);

  const String storedId = doc["installed"]["appId"].is<const char*>() ? doc["installed"]["appId"].as<String>() : "";
  const String storedHash = doc["installed"]["sha256"].is<const char*>() ? doc["installed"]["sha256"].as<String>() : "";

  return storedId == appId && storedHash == sha256;
}

bool AppLoader::switchPartition() {
  const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
  if (!target) {
    Serial.printf("[%lu] [AppLoader] No OTA partition available\n", millis());
    return false;
  }

  const esp_err_t err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [AppLoader] Failed to set boot partition: %d\n", millis(), err);
    return false;
  }

  Serial.printf("[%lu] [AppLoader] Boot partition set: %s\n", millis(), target->label);
  return true;
}

bool AppLoader::bootApp(const String& binPath, const String& appId, ProgressCallback callback) {
  if (!isSDReady()) {
    Serial.printf("[%lu] [AppLoader] SD card not ready, cannot boot app\n", millis());
    return false;
  }

  const String sha256 = calculateFileSHA256(binPath);
  if (sha256.isEmpty()) {
    Serial.printf("[%lu] [AppLoader] Failed to calculate SHA256 for: %s\n", millis(), binPath.c_str());
    return false;
  }

  Serial.printf("[%lu] [AppLoader] App id=%s sha256=%s\n", millis(), appId.c_str(), sha256.c_str());

  if (isAppInstalled(appId, sha256)) {
    Serial.printf("[%lu] [AppLoader] App already installed, switching partition\n", millis());
    if (!switchPartition()) {
      return false;
    }
    Serial.printf("[%lu] [AppLoader] Rebooting...\n", millis());
    esp_restart();
    return true;
  }

  Serial.printf("[%lu] [AppLoader] App not installed or updated, flashing...\n", millis());
  const bool ok = flashApp(binPath, callback);
  if (!ok) {
    return false;
  }

  JsonDocument doc;
  JsonObject installed = doc["installed"].to<JsonObject>();
  installed["appId"] = appId;
  installed["sha256"] = sha256;
  installed["binPath"] = binPath;
  installed["installedMs"] = millis();

  if (!saveInstalledState(doc)) {
    Serial.printf("[%lu] [AppLoader] Warning: failed to save installed state\n", millis());
  }

  Serial.printf("[%lu] [AppLoader] Rebooting...\n", millis());
  esp_restart();
  return true;
}

String AppLoader::buildManifestPath(const String& appDir) const {
  String path = appDir;
  if (!path.endsWith("/")) {
    path += "/";
  }
  path += MANIFEST_FILENAME;
  return path;
}

bool AppLoader::isSDReady() const { return SdMan.ready(); }

}  // namespace CrossPoint
