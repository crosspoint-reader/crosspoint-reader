#pragma once

#include <FeatureFlags.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <string>

#if ENABLE_BLE_WIFI_PROVISIONING
class BLECharacteristic;
class BLEServer;
class BLEService;

/**
 * Lightweight BLE GATT endpoint for provisioning WiFi credentials.
 *
 * Accepted payload formats:
 * - JSON: {"ssid":"MyWiFi","password":"secret"}
 * - WiFi QR-style text: WIFI:T:WPA;S:MyWiFi;P:secret;;
 * - CSV: ssid,password
 * - Two lines: ssid\npassword
 */
class BleWifiProvisioner final {
 public:
  BleWifiProvisioner();
  ~BleWifiProvisioner();

  bool start(const std::string& deviceName = "CrossPoint-WiFi");
  void stop();

  bool isRunning() const { return running.load(); }
  bool takeCredentials(std::string& ssidOut, std::string& passwordOut);
  std::string getStatusMessage() const;

 private:
  class CredentialCharacteristicCallbacks;
  class ServerDisconnectCallbacks;

  void handleIncomingPayload(const std::string& payload);
  void setStatusMessage(const std::string& message);
  bool parsePayload(const std::string& payload, std::string& ssidOut, std::string& passwordOut) const;
  bool parseJsonPayload(const std::string& payload, std::string& ssidOut, std::string& passwordOut) const;
  bool parseWifiQrPayload(const std::string& payload, std::string& ssidOut, std::string& passwordOut) const;
  bool parseDelimitedPayload(const std::string& payload, std::string& ssidOut, std::string& passwordOut) const;
  static std::string trim(const std::string& input);

  SemaphoreHandle_t stateMutex = nullptr;
  std::atomic<bool> running{false};
  bool credentialsReady = false;
  std::string pendingSsid;
  std::string pendingPassword;
  std::string statusMessage = "Waiting for credentials";

  BLEServer* server = nullptr;
  BLEService* service = nullptr;
  BLECharacteristic* characteristic = nullptr;
  CredentialCharacteristicCallbacks* callbacks = nullptr;
  ServerDisconnectCallbacks* serverCallbacks = nullptr;
};
#else
class BleWifiProvisioner final {
 public:
  BleWifiProvisioner() = default;
  ~BleWifiProvisioner() = default;

  bool start(const std::string& deviceName = "CrossPoint-WiFi") {
    (void)deviceName;
    return false;
  }
  void stop() {}

  bool isRunning() const { return false; }
  bool takeCredentials(std::string& ssidOut, std::string& passwordOut) {
    (void)ssidOut;
    (void)passwordOut;
    return false;
  }
  std::string getStatusMessage() const { return "BLE provisioning disabled in this build"; }
};
#endif
