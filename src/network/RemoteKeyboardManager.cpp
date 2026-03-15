#include "RemoteKeyboardManager.h"

#include <DNSServer.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "network/BackgroundWebServer.h"
#include "network/BackgroundWifiService.h"
#include "network/CrossPointWebServer.h"
#include "util/NetworkNames.h"

namespace network {
namespace {

constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr uint16_t DNS_PORT = 53;
constexpr unsigned long ANDROID_HEARTBEAT_WINDOW_MS = 4000;

bool hasStaWifiConnection() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

const char* accessModeHelperText(const RemoteKeyboardManager::AccessMode mode) {
  switch (mode) {
    case RemoteKeyboardManager::AccessMode::AndroidApp:
      return "Android app ready. Type on your phone and press Send.";
    case RemoteKeyboardManager::AccessMode::WifiQr:
      return "Scan the QR code to type from your phone browser.";
    case RemoteKeyboardManager::AccessMode::HotspotStarting:
      return "Starting hotspot for remote keyboard...";
    case RemoteKeyboardManager::AccessMode::HotspotQr:
      return "Scan to join the hotspot, then type on your phone.";
    case RemoteKeyboardManager::AccessMode::None:
    default:
      return "Use the on-device keyboard.";
  }
}

bool isAndroidClient(const char* client) {
  return client != nullptr && client[0] != '\0' && strcasecmp(client, "android") == 0;
}

}  // namespace

RemoteKeyboardManager& RemoteKeyboardManager::getInstance() {
  static RemoteKeyboardManager instance;
  return instance;
}

RemoteKeyboardManager::RemoteKeyboardManager() : mutex(xSemaphoreCreateMutexStatic(&mutexBuffer)) {}

bool RemoteKeyboardManager::hasRecentAndroidHeartbeatLocked() const {
  return session.lastAndroidHeartbeatMs > 0 && (millis() - session.lastAndroidHeartbeatMs) <= ANDROID_HEARTBEAT_WINDOW_MS;
}

std::string RemoteKeyboardManager::normalizeTextLocked(const std::string& text) const {
  std::string normalized;
  normalized.reserve(text.size());
  for (const char c : text) {
    normalized.push_back((c == '\r' || c == '\n') ? ' ' : c);
  }
  if (session.maxLength > 0 && normalized.size() > session.maxLength) {
    normalized.resize(session.maxLength);
  }
  return normalized;
}

std::string RemoteKeyboardManager::buildSessionUrlLocked(const bool useHotspot) const {
  if (!session.active || session.sessionId == 0) {
    return "";
  }

  String host;
  if (useHotspot) {
    host = hotspotIp.c_str();
  } else {
    host = WiFi.localIP().toString();
  }
  if (host.isEmpty()) {
    return "";
  }

  return std::string("http://") + host.c_str() + "/remote-keyboard?session=" + std::to_string(session.sessionId);
}

void RemoteKeyboardManager::startBackgroundWifiLocked() {
  if (session.startedBackgroundWifi || BG_WIFI.isRunning() || BackgroundWebServer::getInstance().isRunning()) {
    return;
  }
  BG_WIFI.startUsingCurrentConnection();
  session.startedBackgroundWifi = true;
}

void RemoteKeyboardManager::startHotspotLocked() {
  if (session.startedHotspot || hotspotTask != nullptr) {
    return;
  }

  hotspotStopRequested = false;
  hotspotReady = false;
  hotspotFailed = false;
  hotspotSsid.clear();
  hotspotIp.clear();

  if (xTaskCreate(&RemoteKeyboardManager::hotspotTaskEntry, "kbhotspot", 6144, this, 1, &hotspotTask) != pdPASS) {
    hotspotTask = nullptr;
    hotspotFailed = true;
    LOG_ERR("RKB", "Failed to start hotspot task");
    return;
  }
  session.startedHotspot = true;
}

void RemoteKeyboardManager::requestHotspotStop() { hotspotStopRequested = true; }

void RemoteKeyboardManager::refreshAccessStateLocked() {
  if (!session.active) {
    return;
  }
  if (hasRecentAndroidHeartbeatLocked()) {
    return;
  }
  if (hasStaWifiConnection()) {
    startBackgroundWifiLocked();
    return;
  }
  startHotspotLocked();
}

void RemoteKeyboardManager::beginSession(const std::string& title, const std::string& initialText, const size_t maxLength,
                                         const bool isPassword) {
  {
    LockGuard lock(mutex);
    session = SessionState{};
    session.active = true;
    session.sessionId = ++sessionCounter;
    session.title = title;
    session.maxLength = maxLength;
    session.isPassword = isPassword;
    session.text = initialText;
    session.text = normalizeTextLocked(session.text);
    refreshAccessStateLocked();
  }
}

void RemoteKeyboardManager::endSession() {
  bool stopBgWifi = false;
  bool stopHotspot = false;
  {
    LockGuard lock(mutex);
    stopBgWifi = session.startedBackgroundWifi;
    stopHotspot = session.startedHotspot;
    session = SessionState{};
  }

  if (stopBgWifi) {
    BG_WIFI.stop(false);
  }
  if (stopHotspot) {
    requestHotspotStop();
    const unsigned long deadline = millis() + 3000;
    while (hotspotTask != nullptr && millis() < deadline) {
      delay(10);
    }
    if (hotspotTask != nullptr) {
      vTaskDelete(hotspotTask);
      hotspotTask = nullptr;
    }
    hotspotReady = false;
    hotspotFailed = false;
    hotspotSsid.clear();
    hotspotIp.clear();
  }
}

void RemoteKeyboardManager::setLocalText(const std::string& text) {
  LockGuard lock(mutex);
  if (!session.active) {
    return;
  }
  session.text = normalizeTextLocked(text);
}

bool RemoteKeyboardManager::consumeRemoteUpdate(std::string& outText, bool& complete, bool& cancel) {
  LockGuard lock(mutex);
  if (!session.active) {
    return false;
  }

  if (session.remoteRevision == session.consumedRemoteRevision && !session.pendingComplete && !session.pendingCancel) {
    return false;
  }

  outText = session.text;
  complete = session.pendingComplete;
  cancel = session.pendingCancel;
  session.consumedRemoteRevision = session.remoteRevision;
  session.pendingComplete = false;
  session.pendingCancel = false;
  return true;
}

RemoteKeyboardManager::SessionSnapshot RemoteKeyboardManager::heartbeat(const char* client) {
  SessionSnapshot snapshot;
  LockGuard lock(mutex);
  if (isAndroidClient(client)) {
    session.lastAndroidHeartbeatMs = millis();
  }
  refreshAccessStateLocked();

  snapshot.active = session.active;
  snapshot.sessionId = session.sessionId;
  snapshot.title = session.title;
  snapshot.text = session.text;
  snapshot.isPassword = session.isPassword;
  snapshot.maxLength = session.maxLength;
  snapshot.androidConnected = hasRecentAndroidHeartbeatLocked();
  return snapshot;
}

bool RemoteKeyboardManager::applyRemoteUpdate(const uint32_t sessionId, const std::string& text, const bool complete,
                                              const bool cancel, const char* client, std::string* errorOut) {
  LockGuard lock(mutex);
  if (!session.active) {
    if (errorOut != nullptr) {
      *errorOut = "No keyboard session is active";
    }
    return false;
  }
  if (session.sessionId != sessionId) {
    if (errorOut != nullptr) {
      *errorOut = "Keyboard session changed. Refresh and try again.";
    }
    return false;
  }

  if (isAndroidClient(client)) {
    session.lastAndroidHeartbeatMs = millis();
  }

  session.text = normalizeTextLocked(text);
  session.remoteRevision++;
  session.pendingComplete = complete;
  session.pendingCancel = cancel;
  return true;
}

RemoteKeyboardManager::UiState RemoteKeyboardManager::getUiState() {
  UiState state;
  LockGuard lock(mutex);
  if (!session.active) {
    return state;
  }

  refreshAccessStateLocked();

  state.active = true;
  state.sessionId = session.sessionId;
  state.title = session.title;
  state.text = session.text;

  if (hasRecentAndroidHeartbeatLocked()) {
    state.accessMode = AccessMode::AndroidApp;
  } else if (hasStaWifiConnection()) {
    state.accessMode = AccessMode::WifiQr;
    state.url = buildSessionUrlLocked(false);
    state.qrPayload = state.url;
  } else if (hotspotReady) {
    state.accessMode = AccessMode::HotspotQr;
    state.ssid = hotspotSsid;
    state.url = buildSessionUrlLocked(true);
    state.qrPayload = std::string("WIFI:S:") + hotspotSsid + ";;";
  } else if (hotspotTask != nullptr && !hotspotFailed) {
    state.accessMode = AccessMode::HotspotStarting;
  } else {
    state.accessMode = AccessMode::None;
  }

  state.helperText = accessModeHelperText(state.accessMode);
  return state;
}

bool RemoteKeyboardManager::isSessionActive() const {
  LockGuard lock(mutex);
  return session.active;
}

bool RemoteKeyboardManager::shouldRedirectRootToKeyboard() const {
  LockGuard lock(mutex);
  return session.active && hotspotReady;
}

bool RemoteKeyboardManager::blocksBackgroundServer() const {
  LockGuard lock(mutex);
  return session.active && session.startedHotspot;
}

uint32_t RemoteKeyboardManager::activeSessionId() const {
  LockGuard lock(mutex);
  return session.active ? session.sessionId : 0;
}

void RemoteKeyboardManager::hotspotTaskEntry(void* arg) {
  static_cast<RemoteKeyboardManager*>(arg)->runHotspotTask();
}

void RemoteKeyboardManager::runHotspotTask() {
  DNSServer dnsServer;
  CrossPointWebServer server;

  char apSsid[40];
  NetworkNames::getApSsid(apSsid, sizeof(apSsid));

  WiFi.mode(WIFI_AP);
  delay(100);

  if (!WiFi.softAP(apSsid, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    LOG_ERR("RKB", "Failed to start remote keyboard hotspot");
    hotspotFailed = true;
    hotspotTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  delay(100);

  const IPAddress apIp = WiFi.softAPIP();
  hotspotSsid = apSsid;
  hotspotIp = apIp.toString().c_str();
  hotspotReady = true;

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIp);

  server.begin();
  if (!server.isRunning()) {
    LOG_ERR("RKB", "Failed to start remote keyboard web server");
    hotspotReady = false;
    hotspotFailed = true;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    hotspotTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  while (!hotspotStopRequested) {
    dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  hotspotReady = false;
  hotspotFailed = false;
  hotspotTask = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace network
