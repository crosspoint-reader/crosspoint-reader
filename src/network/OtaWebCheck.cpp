#include "network/OtaWebCheck.h"

#include <FeatureFlags.h>

#if ENABLE_OTA_UPDATES
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>

#include "network/OtaUpdater.h"

namespace {

enum class OtaWebCheckState { Idle, Checking, Done };

struct OtaWebCheckData {
  std::atomic<OtaWebCheckState> state{OtaWebCheckState::Idle};
  bool available = false;
  std::string latestVersion;
  std::string message;
  int errorCode = 0;
};

OtaWebCheckData otaWebCheckData;
std::mutex otaWebCheckDataMutex;

void otaWebCheckTask(void* param) {
  auto* updater = static_cast<OtaUpdater*>(param);
  const auto result = updater->checkForUpdate();

  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    otaWebCheckData.errorCode = static_cast<int>(result);
    if (result == OtaUpdater::OK) {
      otaWebCheckData.available = updater->isUpdateNewer();
      otaWebCheckData.latestVersion = updater->getLatestVersion();
      otaWebCheckData.message =
          otaWebCheckData.available ? "Update available. Install from device Settings." : "Already on latest version.";
    } else {
      otaWebCheckData.available = false;
      const String& error = updater->getLastError();
      otaWebCheckData.message = error.length() > 0 ? error.c_str() : "Update check failed";
    }
  }

  otaWebCheckData.state.store(OtaWebCheckState::Done, std::memory_order_release);
  delete updater;
  vTaskDelete(nullptr);
}

}  // namespace
#endif

namespace network {

OtaWebStartResult OtaWebCheck::start() {
#if ENABLE_OTA_UPDATES
  if (otaWebCheckData.state.load(std::memory_order_acquire) == OtaWebCheckState::Checking) {
    return OtaWebStartResult::AlreadyChecking;
  }

  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    otaWebCheckData.available = false;
    otaWebCheckData.latestVersion.clear();
    otaWebCheckData.message = "Checking...";
    otaWebCheckData.errorCode = 0;
  }
  otaWebCheckData.state.store(OtaWebCheckState::Checking, std::memory_order_release);

  auto* updater = new OtaUpdater();
  if (xTaskCreate(otaWebCheckTask, "OtaWebCheckTask", 4096, updater, 1, nullptr) != pdPASS) {
    delete updater;
    otaWebCheckData.state.store(OtaWebCheckState::Idle, std::memory_order_release);
    return OtaWebStartResult::StartTaskFailed;
  }

  return OtaWebStartResult::Started;
#else
  return OtaWebStartResult::Disabled;
#endif
}

OtaWebCheckSnapshot OtaWebCheck::getSnapshot() {
#if ENABLE_OTA_UPDATES
  OtaWebCheckSnapshot snapshot;
  const OtaWebCheckState state = otaWebCheckData.state.load(std::memory_order_acquire);
  snapshot.status = state == OtaWebCheckState::Checking
                        ? OtaWebCheckStatus::Checking
                        : (state == OtaWebCheckState::Done ? OtaWebCheckStatus::Done : OtaWebCheckStatus::Idle);
  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    snapshot.available = otaWebCheckData.available;
    snapshot.latestVersion = otaWebCheckData.latestVersion;
    snapshot.message = otaWebCheckData.message;
    snapshot.errorCode = otaWebCheckData.errorCode;
  }
  return snapshot;
#else
  return {};
#endif
}

}  // namespace network
