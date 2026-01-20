#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declarations to minimize includes when BLE is disabled
#ifdef CONFIG_BT_ENABLED
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#endif

/**
 * Memory-efficient Bluetooth Manager for CrossPoint Reader
 * 
 * Design principles:
 * - Singleton pattern to minimize memory usage
 * - Conditional compilation to avoid BLE overhead when disabled
 * - Minimal RAM footprint (~2-3KB when disabled, ~15KB when enabled)
 * - Lazy initialization only when needed
 * - Clean shutdown to prevent memory leaks
 */
class BluetoothManager {
private:
  // Private constructor for singleton
  BluetoothManager() = default;
  
  // Static instance
  static BluetoothManager instance;
  
  // State tracking (minimal memory usage)
  bool initialized = false;
  bool advertising = false;
  
#ifdef CONFIG_BT_ENABLED
  // BLE components (only allocated when BLE is enabled)
  BLEServer* pServer = nullptr;
  BLEAdvertising* pAdvertising = nullptr;
  
  // Device name (short to save memory)
  static constexpr const char* DEVICE_NAME = "CrossPoint";
#endif

public:
  // Delete copy constructor and assignment
  BluetoothManager(const BluetoothManager&) = delete;
  BluetoothManager& operator=(const BluetoothManager&) = delete;
  
  /**
   * Get singleton instance
   * @return Reference to BluetoothManager instance
   */
  static BluetoothManager& getInstance() { return instance; }
  
  /**
   * Initialize Bluetooth stack
   * @return true if initialization successful, false otherwise
   */
  bool initialize();
  
  /**
   * Shutdown Bluetooth stack to free memory
   */
  void shutdown();
  
  /**
   * Start advertising device
   * @return true if advertising started successfully
   */
  bool startAdvertising();
  
  /**
   * Stop advertising to save power
   */
  void stopAdvertising();
  
  /**
   * Check if Bluetooth is initialized
   * @return true if initialized
   */
  bool isInitialized() const { return initialized; }
  
  /**
   * Check if currently advertising
   * @return true if advertising
   */
  bool isAdvertising() const { return advertising; }
  
  /**
   * Get memory usage information
   * @return Estimated RAM usage in bytes
   */
  size_t getMemoryUsage() const;
  
  /**
   * Force garbage collection to free unused memory
   */
  void collectGarbage();
  
private:
#ifdef CONFIG_BT_ENABLED
  /**
   * Create BLE server with minimal services
   * @return true if server created successfully
   */
  bool createServer();
  
  /**
   * Setup advertising data with minimal payload
   */
  void setupAdvertising();
  
  /**
   * BLE server callbacks (minimal implementation)
   */
  class ServerCallbacks : public BLEServerCallbacks {
  public:
    void onConnect(BLEServer* pServer) override;
    void onDisconnect(BLEServer* pServer) override;
  };
#endif
};

// Convenience macro for accessing the manager
#define BLUETOOTH_MANAGER BluetoothManager::getInstance()