#include "BluetoothManager.h"
#include "BLEKeyboardHandler.h"

#ifdef ARDUINO
#include <Arduino.h>
#include "CrossPointSettings.h"
#endif

// Static instance definition
BluetoothManager BluetoothManager::instance;

// Global accessor for BLEKeyboardHandler used by BLE callbacks
BLEKeyboardHandler* getActiveKeyboardHandler() {
  return BLUETOOTH_MANAGER.getKeyboardHandler();
}

bool BluetoothManager::initialize() {
#ifdef CONFIG_BT_ENABLED
  // Prevent double initialization
  if (initialized) {
    return true;
  }

  Serial.printf("[%lu] [BLE] Initializing Bluetooth\n", millis());

  try {
    // Initialize NimBLE device with minimal configuration
    BLEDevice::init(DEVICE_NAME);

    // Create server if needed
    if (!createServer()) {
      Serial.printf("[%lu] [BLE] Failed to create server\n", millis());
      BLEDevice::deinit();
      return false;
    }

    // Setup advertising
    setupAdvertising();

    initialized = true;
    Serial.printf("[%lu] [BLE] Bluetooth initialized successfully\n", millis());
    Serial.printf("[%lu] [BLE] Free heap after init: %d bytes\n", millis(), ESP.getFreeHeap());

    return true;

  } catch (...) {
    Serial.printf("[%lu] [BLE] Exception during initialization\n", millis());
    BLEDevice::deinit();
    return false;
  }
#else
  return false;
#endif
}

void BluetoothManager::shutdown() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }

  Serial.printf("[%lu] [BLE] Shutting down Bluetooth\n", millis());

  // Stop advertising first
  stopAdvertising();

  // Clean up keyboard handler BEFORE deinitializing BLE stack
  if (pKeyboardHandler) {
    pKeyboardHandler->shutdown();
    delete pKeyboardHandler;
    pKeyboardHandler = nullptr;
  }

  // Now safe to deinitialize BLE device
  BLEDevice::deinit();

  // Clean up pointers (invalidated by deinit)
  pServer = nullptr;
  pAdvertising = nullptr;

  initialized = false;
  advertising = false;

  Serial.printf("[%lu] [BLE] Bluetooth shutdown complete\n", millis());
  Serial.printf("[%lu] [BLE] Free heap after shutdown: %d bytes\n", millis(), ESP.getFreeHeap());
#endif
}

bool BluetoothManager::startAdvertising() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized || advertising) {
    return advertising;
  }

  if (pAdvertising) {
    pAdvertising->start();
    advertising = true;
    Serial.printf("[%lu] [BLE] Advertising started\n", millis());
    return true;
  }

  Serial.printf("[%lu] [BLE] Failed to start advertising\n", millis());
  return false;
#else
  return false;
#endif
}

void BluetoothManager::stopAdvertising() {
#ifdef CONFIG_BT_ENABLED
  if (!advertising || !pAdvertising) {
    return;
  }

  pAdvertising->stop();
  advertising = false;
  Serial.printf("[%lu] [BLE] Advertising stopped\n", millis());
#endif
}

size_t BluetoothManager::getMemoryUsage() const {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return sizeof(*this);
  }

  size_t baseUsage = sizeof(*this);
  size_t stackUsage = 0;

  // NimBLE stack typically uses 12-15KB RAM
  if (pServer) {
    stackUsage += 12288;  // Conservative estimate
  }

  // Add keyboard handler usage
  if (pKeyboardHandler) {
    stackUsage += pKeyboardHandler->getMemoryUsage();
  }

  return baseUsage + stackUsage;
#else
  return sizeof(*this);
#endif
}

BLEKeyboardHandler* BluetoothManager::getKeyboardHandler() const {
#ifdef CONFIG_BT_ENABLED
  return pKeyboardHandler;
#else
  return nullptr;
#endif
}

void BluetoothManager::collectGarbage() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }

  NimBLEDevice::getScan()->clearResults();

  Serial.printf("[%lu] [BLE] Garbage collection complete\n", millis());
#endif
}

#ifdef CONFIG_BT_ENABLED
bool BluetoothManager::createServer() {
  try {
    pServer = BLEDevice::createServer();
    if (!pServer) {
      return false;
    }

    // Set callbacks
    pServer->setCallbacks(new ServerCallbacks());

    // Initialize keyboard handler if enabled in settings
    if (SETTINGS.bluetoothKeyboardEnabled == CrossPointSettings::BLUETOOTH_KEYBOARD_MODE::KBD_ENABLED) {
      pKeyboardHandler = new BLEKeyboardHandler();
      if (!pKeyboardHandler->initialize(pServer)) {
        Serial.printf("[%lu] [BLE] Failed to initialize keyboard handler\n", millis());
        delete pKeyboardHandler;
        pKeyboardHandler = nullptr;
      }
    }

    return true;

  } catch (...) {
    pServer = nullptr;
    if (pKeyboardHandler) {
      delete pKeyboardHandler;
      pKeyboardHandler = nullptr;
    }
    return false;
  }
}

void BluetoothManager::setupAdvertising() {
  if (!pServer) {
    return;
  }

  pAdvertising = BLEDevice::getAdvertising();
  if (!pAdvertising) {
    return;
  }

  // Minimal advertising configuration
  pAdvertising->addServiceUUID(NimBLEUUID((uint16_t)0x1800));  // Generic Access
  pAdvertising->enableScanResponse(false);                      // Save power and memory
  pAdvertising->setMinInterval(0x20);                           // 20ms min interval
  pAdvertising->setMaxInterval(0x40);                           // 40ms max interval
}

void BluetoothManager::ServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  (void)connInfo;
  Serial.printf("[%lu] [BLE] Device connected\n", millis());
}

void BluetoothManager::ServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
  (void)connInfo;
  Serial.printf("[%lu] [BLE] Device disconnected (reason: %d)\n", millis(), reason);

  // Restart advertising
  NimBLEDevice::getAdvertising()->start();
}
#endif
