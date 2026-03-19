#include "BleWifiProvisioner.h"

#if ENABLE_BLE_WIFI_PROVISIONING

#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace {
constexpr const char* kServiceUuid = "41cb0001-b8f4-4e4a-9f49-ecb9d6fd4b90";
constexpr const char* kCharacteristicUuid = "41cb0002-b8f4-4e4a-9f49-ecb9d6fd4b90";
constexpr size_t kMaxPayloadSize = 320;
}  // namespace

class BleWifiProvisioner::ServerDisconnectCallbacks : public BLEServerCallbacks {
 public:
  void onDisconnect(BLEServer* pServer) override {
    (void)pServer;
    BLEDevice::startAdvertising();
    LOG_DBG("BLE", "Client disconnected — restarted advertising");
  }
};

class BleWifiProvisioner::CredentialCharacteristicCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit CredentialCharacteristicCallbacks(BleWifiProvisioner* owner) : owner(owner) {}

  void onWrite(BLECharacteristic* characteristic) override {
    if (!owner || !characteristic) {
      return;
    }

    std::string payload = characteristic->getValue();
    owner->handleIncomingPayload(payload);
  }

 private:
  BleWifiProvisioner* owner;
};

BleWifiProvisioner::BleWifiProvisioner() { stateMutex = xSemaphoreCreateMutex(); }

BleWifiProvisioner::~BleWifiProvisioner() {
  stop();
  if (stateMutex) {
    vSemaphoreDelete(stateMutex);
    stateMutex = nullptr;
  }
}

bool BleWifiProvisioner::start(const std::string& deviceName) {
  if (running.load()) {
    return true;
  }
  if (!stateMutex) {
    return false;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  credentialsReady = false;
  pendingSsid.clear();
  pendingPassword.clear();
  statusMessage = "Starting BLE...";
  xSemaphoreGive(stateMutex);

  BLEDevice::init(deviceName.c_str());
  server = BLEDevice::createServer();
  if (!server) {
    setStatusMessage("BLE server create failed");
    BLEDevice::deinit(true);
    return false;
  }

  serverCallbacks = new ServerDisconnectCallbacks();
  server->setCallbacks(serverCallbacks);

  service = server->createService(kServiceUuid);
  if (!service) {
    setStatusMessage("BLE service create failed");
    BLEDevice::deinit(true);
    server = nullptr;
    return false;
  }

  characteristic = service->createCharacteristic(kCharacteristicUuid,
                                                 BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  if (!characteristic) {
    setStatusMessage("BLE characteristic create failed");
    BLEDevice::deinit(true);
    server = nullptr;
    service = nullptr;
    return false;
  }

  callbacks = new CredentialCharacteristicCallbacks(this);
  characteristic->setCallbacks(callbacks);
  characteristic->setValue("Send WiFi credentials payload");

  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  running.store(true);
  setStatusMessage("BLE ready: write SSID/password");
  LOG_DBG("BLE", "WiFi provisioning BLE started");
  return true;
}

void BleWifiProvisioner::stop() {
  if (!running.exchange(false)) {
    return;
  }

  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);

  if (callbacks) {
    delete callbacks;
    callbacks = nullptr;
  }
  if (serverCallbacks) {
    delete serverCallbacks;
    serverCallbacks = nullptr;
  }
  server = nullptr;
  service = nullptr;
  characteristic = nullptr;
  setStatusMessage("BLE stopped");
  LOG_DBG("BLE", "WiFi provisioning BLE stopped");
}

bool BleWifiProvisioner::takeCredentials(std::string& ssidOut, std::string& passwordOut) {
  if (!stateMutex) {
    return false;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (!credentialsReady) {
    xSemaphoreGive(stateMutex);
    return false;
  }

  ssidOut = pendingSsid;
  passwordOut = pendingPassword;
  credentialsReady = false;
  pendingSsid.clear();
  pendingPassword.clear();
  xSemaphoreGive(stateMutex);
  return true;
}

std::string BleWifiProvisioner::getStatusMessage() const {
  if (!stateMutex) {
    return "BLE unavailable";
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  std::string message = statusMessage;
  xSemaphoreGive(stateMutex);
  return message;
}

void BleWifiProvisioner::handleIncomingPayload(const std::string& payload) {
  if (payload.empty()) {
    setStatusMessage("Empty payload");
    return;
  }

  if (payload.size() > kMaxPayloadSize) {
    setStatusMessage("Payload too large");
    return;
  }

  std::string ssid;
  std::string password;
  if (!parsePayload(payload, ssid, password)) {
    setStatusMessage("Invalid payload");
    return;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingSsid = ssid;
  pendingPassword = password;
  credentialsReady = true;
  statusMessage = "Credentials received";
  xSemaphoreGive(stateMutex);

  LOG_DBG("BLE", "Received WiFi credentials over BLE for SSID: %s", ssid.c_str());
}

void BleWifiProvisioner::setStatusMessage(const std::string& message) {
  if (!stateMutex) {
    return;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  statusMessage = message;
  xSemaphoreGive(stateMutex);
}

bool BleWifiProvisioner::parsePayload(const std::string& payload, std::string& ssidOut,
                                      std::string& passwordOut) const {
  return parseJsonPayload(payload, ssidOut, passwordOut) || parseWifiQrPayload(payload, ssidOut, passwordOut) ||
         parseDelimitedPayload(payload, ssidOut, passwordOut);
}

bool BleWifiProvisioner::parseJsonPayload(const std::string& payload, std::string& ssidOut,
                                          std::string& passwordOut) const {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return false;
  }

  const char* ssid = doc["ssid"];
  if (!ssid || strlen(ssid) == 0) {
    return false;
  }

  const char* password = doc["password"] | "";
  ssidOut = ssid;
  passwordOut = password ? password : "";
  return true;
}

bool BleWifiProvisioner::parseWifiQrPayload(const std::string& payload, std::string& ssidOut,
                                            std::string& passwordOut) const {
  if (payload.rfind("WIFI:", 0) != 0) {
    return false;
  }

  auto extractField = [&](const std::string& prefix) -> std::string {
    size_t start = payload.find(prefix);
    if (start == std::string::npos) return "";
    start += prefix.length();

    std::string result;
    for (size_t i = start; i < payload.length(); ++i) {
      char c = payload[i];
      if (c == '\\' && i + 1 < payload.length()) {
        result += payload[++i];  // Skip escape and add next char
      } else if (c == ';') {
        break;  // End of field
      } else {
        result += c;
      }
    }
    return result;
  };

  ssidOut = extractField("S:");
  passwordOut = extractField("P:");
  return !ssidOut.empty();
}

bool BleWifiProvisioner::parseDelimitedPayload(const std::string& payload, std::string& ssidOut,
                                               std::string& passwordOut) const {
  size_t delimiter = payload.find(',');
  if (delimiter == std::string::npos) {
    delimiter = payload.find('\n');
  }
  if (delimiter == std::string::npos) {
    return false;
  }

  ssidOut = trim(payload.substr(0, delimiter));
  passwordOut = trim(payload.substr(delimiter + 1));
  return !ssidOut.empty();
}

std::string BleWifiProvisioner::trim(const std::string& input) {
  auto start = std::find_if_not(input.begin(), input.end(), [](const unsigned char c) { return std::isspace(c); });
  auto end =
      std::find_if_not(input.rbegin(), input.rend(), [](const unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return "";
  }
  return std::string(start, end);
}

#endif
