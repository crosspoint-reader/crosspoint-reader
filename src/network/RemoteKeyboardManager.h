#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace network {

class RemoteKeyboardManager {
 public:
  enum class AccessMode : uint8_t { None, AndroidApp, WifiQr, HotspotStarting, HotspotQr };

  struct SessionSnapshot {
    bool active = false;
    uint32_t sessionId = 0;
    std::string title;
    std::string text;
    bool isPassword = false;
    size_t maxLength = 0;
    bool androidConnected = false;
  };

  struct UiState {
    bool active = false;
    uint32_t sessionId = 0;
    AccessMode accessMode = AccessMode::None;
    std::string title;
    std::string text;
    std::string helperText;
    std::string qrPayload;
    std::string ssid;
    std::string url;
  };

  static RemoteKeyboardManager& getInstance();

  void beginSession(const std::string& title, const std::string& initialText, size_t maxLength, bool isPassword);
  void endSession();
  void setLocalText(const std::string& text);

  bool consumeRemoteUpdate(std::string& outText, bool& complete, bool& cancel);

  SessionSnapshot heartbeat(const char* client);
  bool applyRemoteUpdate(uint32_t sessionId, const std::string& text, bool complete, bool cancel, const char* client,
                         std::string* errorOut);

  UiState getUiState();
  bool isSessionActive() const;
  bool shouldRedirectRootToKeyboard() const;
  bool blocksBackgroundServer() const;
  uint32_t activeSessionId() const;

 private:
  RemoteKeyboardManager();
  RemoteKeyboardManager(const RemoteKeyboardManager&) = delete;
  RemoteKeyboardManager& operator=(const RemoteKeyboardManager&) = delete;

  struct SessionState {
    bool active = false;
    uint32_t sessionId = 0;
    std::string title;
    std::string text;
    size_t maxLength = 0;
    bool isPassword = false;
    uint32_t remoteRevision = 0;
    uint32_t consumedRemoteRevision = 0;
    bool pendingComplete = false;
    bool pendingCancel = false;
    unsigned long lastAndroidHeartbeatMs = 0;
    bool startedBackgroundWifi = false;
    bool startedHotspot = false;
  };

  class LockGuard {
   public:
    explicit LockGuard(SemaphoreHandle_t mutex) : mutex(mutex) {
      if (mutex != nullptr) {
        xSemaphoreTake(mutex, portMAX_DELAY);
      }
    }
    ~LockGuard() {
      if (mutex != nullptr) {
        xSemaphoreGive(mutex);
      }
    }

   private:
    SemaphoreHandle_t mutex;
  };

  static void hotspotTaskEntry(void* arg);
  void runHotspotTask();

  void refreshAccessStateLocked();
  void startBackgroundWifiLocked();
  void startHotspotLocked();
  void requestHotspotStop();
  bool hasRecentAndroidHeartbeatLocked() const;
  std::string normalizeTextLocked(const std::string& text) const;
  std::string buildSessionUrlLocked(bool useHotspot) const;

  mutable StaticSemaphore_t mutexBuffer{};
  mutable SemaphoreHandle_t mutex = nullptr;

  SessionState session;
  uint32_t sessionCounter = 0;

  TaskHandle_t hotspotTask = nullptr;
  volatile bool hotspotStopRequested = false;
  volatile bool hotspotReady = false;
  volatile bool hotspotFailed = false;
  std::string hotspotSsid;
  std::string hotspotIp;
};

}  // namespace network

#define REMOTE_KEYBOARD network::RemoteKeyboardManager::getInstance()
