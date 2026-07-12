#pragma once
#include <cstdint>

/**
 * Manages WiFi/BLE radio coexistence on ESP32-C3.
 * The radio is shared — WiFi and BLE cannot run simultaneously.
 * Call ensureWifi() before any WiFi operation and ensureBle() before any BLE operation.
 */
class RadioManager {
 public:
  enum class RadioState { OFF, WIFI, BLE };

  static RadioManager& getInstance() {
    static RadioManager instance;
    return instance;
  }

  // Ensure WiFi is available (deinits BLE if active)
  bool ensureWifi();

  // Ensure BLE is available (deinits WiFi if active)
  bool ensureBle();

  // Shut down all radios
  void shutdown();

  RadioState getState() const { return state; }

  // Check if disclaimer has been acknowledged (stored in NVS)
  bool isDisclaimerAcknowledged() const;
  void setDisclaimerAcknowledged();

 private:
  RadioManager() = default;
  RadioState state = RadioState::OFF;

  void deinitWifi();
  void deinitBle();
};

#define RADIO RadioManager::getInstance()
