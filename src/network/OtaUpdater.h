#pragma once

#include <functional>
#include <string>

// Avoid pulling in esp_https_ota.h here — it transitively includes lwip/sockets.h
// which defines INADDR_NONE as a numeric macro, conflicting with Arduino's IPAddress.h.
typedef void* esp_https_ota_handle_t;

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  bool render = false;
  esp_https_ota_handle_t otaHandle = nullptr;
  bool cancelRequested = false;

 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    UPDATE_CANCELLED,
    UPDATE_IN_PROGRESS,
    VALIDATE_FAILED,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  bool getRender() const { return render; }
  void clearRender() { render = false; }

  bool isUpdateInProgress() const { return otaHandle != nullptr; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError beginInstallUpdate();
  OtaUpdaterError performInstallUpdateStep();
  void cancelUpdate();
  void cleanupUpdate();

 private:
  static int forceSetOtaBootPartition();
};
