#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declarations for conditional compilation
#ifdef CONFIG_BT_ENABLED
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <NimBLEService.h>
#include <NimBLEHIDDevice.h>
#include <HIDKeyboardTypes.h>
#endif

/**
 * Memory-efficient BLE Keyboard Handler for CrossPoint Reader
 * 
 * Design principles:
 * - Minimal RAM footprint (~1KB when active)
 * - Efficient key mapping to existing CrossPoint buttons
 * - Robust error handling and automatic recovery
 * - Power-optimized with idle timeouts
 */
class BLEKeyboardHandler {
private:
  // Private constructor for singleton pattern
  BLEKeyboardHandler() = default;
  
  // Static instance
  static BLEKeyboardHandler instance;
  
  // State tracking (minimal memory usage)
  bool initialized = false;
  bool connected = false;
  uint32_t lastActivityTime = 0;
  
#ifdef CONFIG_BT_ENABLED
  // BLE components (only allocated when needed)
  NimBLEServer* pServer = nullptr;
  NimBLEService* pService = nullptr;
  NimBLECharacteristic* pInputCharacteristic = nullptr;
  
  // Keyboard report buffer (minimal size for our needs)
  uint8_t keyboardReport[8] = {0};
  
  // Key debounce timing
  static constexpr uint32_t DEBOUNCE_MS = 50;
  static constexpr uint32_t IDLE_TIMEOUT_MS = 30000; // 30 seconds
#endif

public:
  // Delete copy constructor and assignment
  BLEKeyboardHandler(const BLEKeyboardHandler&) = delete;
  BLEKeyboardHandler& operator=(const BLEKeyboardHandler&) = delete;
  
  /**
   * Get singleton instance
   * @return Reference to BLEKeyboardHandler instance
   */
  static BLEKeyboardHandler& getInstance() { return instance; }
  
  /**
   * Initialize BLE Keyboard service
   * @param server Pointer to existing BLE server
   * @return true if initialization successful
   */
  bool initialize(NimBLEServer* server);
  
  /**
   * Shutdown keyboard service and free memory
   */
  void shutdown();
  
  /**
   * Process incoming keyboard data
   * @param data Raw keyboard report data
   * @param length Length of keyboard report
   */
  void processKeyboardReport(const uint8_t* data, size_t length);
  
  /**
   * Check if keyboard is connected
   * @return true if connected
   */
  bool isConnected() const { return connected; }
  
  /**
   * Check if initialized
   * @return true if initialized
   */
  bool isInitialized() const { return initialized; }
  
  /**
   * Update idle timeout and power management
   */
  void update();
  
  /**
   * Get memory usage information
   * @return Estimated RAM usage in bytes
   */
  size_t getMemoryUsage() const;
  
  /**
   * Check for keyboard inactivity
   * @return true if idle longer than timeout
   */
  bool isIdle() const;

private:
#ifdef CONFIG_BT_ENABLED
  /**
   * Setup HID descriptor for keyboard
   * @return true if successful
   */
  bool setupHidDescriptor();
  
  /**
   * Convert keyboard scancode to CrossPoint button
   * @param scancode USB HID scancode
   * @return Mapped button ID or -1 if unmapped
   */
  int mapScancodeToButton(uint8_t scancode) const;
  
  /**
   * Handle modifier keys (Shift, Ctrl, etc.)
   * @param modifiers Modifier byte from keyboard report
   */
  void handleModifiers(uint8_t modifiers);
  
  /**
   * BLE keyboard callbacks
   */
  class KeyboardCallbacks : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* pCharacteristic);
    void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc);
    void onUnsubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc);
  };
#endif
};

// Convenience macro
#define BLE_KEYBOARD BLEKeyboardHandler::getInstance()