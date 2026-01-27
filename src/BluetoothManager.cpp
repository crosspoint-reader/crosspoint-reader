#include "BluetoothManager.h"
#include "BLEKeyboardHandler.h"

// Platform-specific includes
#ifdef ARDUINO
#include <Arduino.h>
#include "CrossPointSettings.h"
#else
// For static analysis, provide minimal declarations
extern "C" {
  unsigned long millis();
  int ESP_getFreeHeap();
  void Serial_printf(const char* format, ...);
}
#define Serial Serial_printf
#endif

// Static instance definition
BluetoothManager BluetoothManager::instance;

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
    return false;
  }
#else
  Serial.printf("[%lu] [BLE] Bluetooth disabled in build\n", millis());
  return false;
#endif
}

void BluetoothManager::shutdown() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }
  
  Serial.printf("[%lu] [BLE] Shutting down Bluetooth\n", millis());
  
  // Stop advertising
  stopAdvertising();
  
  // Deinitialize BLE device
  BLEDevice::deinit();
  
  // Clean up keyboard handler first
  if (pKeyboardHandler) {
    pKeyboardHandler->shutdown();
    delete pKeyboardHandler;
    pKeyboardHandler = nullptr;
  }
  
  // Clean up pointers
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
  
  if (pAdvertising && pAdvertising->start()) {
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
    return sizeof(*this);  // Base object size (~20 bytes)
  }
  
  // Estimate BLE stack memory usage
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
  return sizeof(*this);  // Minimal usage when disabled
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
  
  // Force garbage collection in NimBLE
  NimBLEDevice::getScan()->clearResults();
  
  Serial.printf("[%lu] [BLE] Garbage collection complete\n", millis());
#endif
}

#ifdef CONFIG_BT_ENABLED
bool BluetoothManager::createServer() {
  try {
    // Create BLE server with minimal configuration
    pServer = BLEDevice::createServer();
    if (!pServer) {
      return false;
    }
    
    // Set callbacks with minimal overhead
    pServer->setCallbacks(new ServerCallbacks());
    
    // Initialize keyboard handler if enabled
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
    pKeyboardHandler = nullptr;
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
  pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x1800));  // Generic Access
  pAdvertising->setScanResponse(false);  // Save power and memory
  pAdvertising->setMinPreferred(0x0);    // No preferred connections
  pAdvertising->setMaxPreferred(0x0);
}

void BluetoothManager::ServerCallbacks::onConnect(BLEServer* pServer) {
  Serial.printf("[%lu] [BLE] Device connected\n", millis());
  
  // Restart advertising for more connections (though we only allow 1)
  BLEDevice::getAdvertising()->start();
}

void BluetoothManager::ServerCallbacks::onDisconnect(BLEServer* pServer) {
  Serial.printf("[%lu] [BLE] Device disconnected\n", millis());
  
  // Restart advertising
  BLEDevice::getAdvertising()->start();
}
#endif