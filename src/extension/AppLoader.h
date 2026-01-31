#pragma once

#include <ArduinoJson.h>
#include <WString.h>
#include <vector>
#include <functional>
#include <SDCardManager.h>

namespace CrossPoint {

/**
 * @brief App manifest data structure
 * 
 * Contains the metadata parsed from app.json files
 */
struct AppManifest {
  String name;           ///< Display name of the app
  String version;        ///< Version string (e.g., "1.0.0")
  String description;    ///< Brief description of the app
  String author;         ///< Author/creator name
  String minFirmware;    ///< Minimum firmware version required

  AppManifest() = default;
  AppManifest(const String& n, const String& v, const String& d, const String& a, const String& f)
      : name(n), version(v), description(d), author(a), minFirmware(f) {}
};

/**
 * @brief Complete app information including manifest and path
 * 
 * Combines the parsed manifest with file system path information
 */
struct AppInfo {
  AppManifest manifest;  ///< The parsed app manifest
  String path;           ///< Full path to the app directory (e.g., "/.crosspoint/apps/test")

  AppInfo() = default;
  AppInfo(const AppManifest& m, const String& p) : manifest(m), path(p) {}
};

/**
 * @brief Utility class for loading and managing apps from SD card
 * 
 * Handles scanning for app manifests in the /.crosspoint/apps directory,
 * parsing JSON manifests, and providing access to app information.
 * 
 * Usage:
 *   AppLoader loader;
 *   std::vector<AppInfo> apps = loader.scanApps();
 *   for (const auto& app : apps) {
 *     Serial.println(app.manifest.name);
 *   }
 */
class AppLoader {
 public:
  AppLoader() = default;
  ~AppLoader() = default;

  using ProgressCallback = std::function<void(size_t written, size_t total)>;

  /**
   * @brief Scan for apps in the /.crosspoint/apps directory
   * 
   * Searches for subdirectories under /.crosspoint/apps and attempts to
   * parse app.json files in each directory. Invalid or missing manifests
   * are skipped gracefully.
   * 
   * @return Vector of AppInfo objects for all valid apps found
   */
  std::vector<AppInfo> scanApps();

  /**
   * @brief Parse an app.json manifest file
   * 
   * Reads and parses a JSON manifest file, extracting the required fields.
   * Logs errors for malformed JSON but does not throw exceptions.
   * 
   * @param path Full path to the app.json file
   * @return AppManifest object with parsed data (empty on failure)
   */
  AppManifest parseManifest(const String& path);

  /**
   * @brief Flash an app binary from SD card to the OTA partition
   *
   * @param binPath Full path to the binary file on SD card
   * @param callback Optional progress callback (values 0-100)
   * @return true if flashing succeeded, false on error
   */
  bool flashApp(const String& binPath, ProgressCallback callback = nullptr);

 private:
  /**
   * @brief Base path for apps directory
   */
  static constexpr const char* APPS_BASE_PATH = "/.crosspoint/apps";

  /**
   * @brief Name of the manifest file in each app directory
   */
  static constexpr const char* MANIFEST_FILENAME = "app.json";

  /**
   * @brief Maximum file size to read for manifest (prevents memory issues)
   */
  static constexpr size_t MAX_MANIFEST_SIZE = 8192;  // 8KB

  /**
   * @brief Helper to build manifest file path from app directory path
   * 
   * @param appDir Path to the app directory
   * @return Full path to the app.json file
   */
  String buildManifestPath(const String& appDir) const;

  /**
   * @brief Check if SD card is ready
   * 
   * @return true if SD card is initialized and ready
   */
  bool isSDReady() const;
};

}  // namespace CrossPoint
