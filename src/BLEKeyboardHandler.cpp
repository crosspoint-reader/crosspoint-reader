#include "BLEKeyboardHandler.h"

// Platform-specific includes
#ifdef ARDUINO
#include "Arduino.h"
#include "MappedInputManager.h"
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
BLEKeyboardHandler BLEKeyboardHandler::instance;

bool BLEKeyboardHandler::initialize(NimBLEServer* server) {
#ifdef CONFIG_BT_ENABLED
  if (initialized || !server) {
    return initialized;
  }
  
  Serial.printf("[%lu] [KBD] Initializing BLE Keyboard\n", millis());
  
  try {
    pServer = server;
    
    // Create custom keyboard service
    pService = pServer->createService("12345678-1234-1234-1234-123456789abc");
    if (!pService) {
      Serial.printf("[%lu] [KBD] Failed to create service\n", millis());
      return false;
    }
    
    // Create input characteristic
    pInputCharacteristic = pService->createCharacteristic(
      "87654321-4321-4321-4321-cba987654321",
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    if (!pInputCharacteristic) {
      Serial.printf("[%lu] [KBD] Failed to create input characteristic\n", millis());
      return false;
    }
    
    // Set callbacks
    pInputCharacteristic->setCallbacks(new KeyboardCallbacks());
    
    // Start HID service
    pHidDevice->startServices();
    
    initialized = true;
    Serial.printf("[%lu] [KBD] BLE Keyboard initialized\n", millis());
    Serial.printf("[%lu] [KBD] Free heap after init: %d bytes\n", millis(), ESP.getFreeHeap());
    
    return true;
    
  } catch (...) {
    Serial.printf("[%lu] [KBD] Exception during initialization\n", millis());
    if (pHidDevice) {
      delete pHidDevice;
      pHidDevice = nullptr;
    }
    return false;
  }
#else
  Serial.printf("[%lu] [KBD] BLE Keyboard disabled in build\n", millis());
  return false;
#endif
}

void BLEKeyboardHandler::shutdown() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }
  
  Serial.printf("[%lu] [KBD] Shutting down BLE Keyboard\n", millis());
  
  connected = false;
  
  if (pHidDevice) {
    delete pHidDevice;
    pHidDevice = nullptr;
  }
  
  pInputCharacteristic = nullptr;
  pServer = nullptr;
  initialized = false;
  
  // Clear keyboard report
  memset(keyboardReport, 0, sizeof(keyboardReport));
  
  Serial.printf("[%lu] [KBD] BLE Keyboard shutdown complete\n", millis());
#endif
}

void BLEKeyboardHandler::processKeyboardReport(const uint8_t* data, size_t length) {
#ifdef CONFIG_BT_ENABLED
  if (!initialized || !data || length < 8) {
    return;
  }
  
  // Debounce check
  uint32_t currentTime = millis();
  if (currentTime - lastActivityTime < DEBOUNCE_MS) {
    return;
  }
  
  lastActivityTime = currentTime;
  
  // Parse keyboard report (HID standard format)
  uint8_t modifiers = data[0];
  uint8_t reserved = data[1];
  uint8_t keycodes[6] = {data[2], data[3], data[4], data[5], data[6], data[7]};
  
  // Handle modifiers first (Shift, Ctrl, Alt)
  handleModifiers(modifiers);
  
  // Process each key
  for (int i = 0; i < 6; i++) {
    if (keycodes[i] != 0) {
      int buttonId = mapScancodeToButton(keycodes[i]);
      if (buttonId >= 0) {
        // Inject mapped button into existing input system
        extern MappedInputManager mappedInputManager;
        mappedInputManager.injectButton(static_cast<MappedInputManager::Button>(buttonId));
      }
    }
  }
#endif
}

void BLEKeyboardHandler::update() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }
  
  // Check for idle timeout
  if (isIdle()) {
    // Optionally reduce power or disconnect after long idle
    if (connected && (millis() - lastActivityTime > IDLE_TIMEOUT_MS * 2)) {
      Serial.printf("[%lu] [KBD] Very long idle, considering disconnect\n", millis());
    }
  }
#endif
}

size_t BLEKeyboardHandler::getMemoryUsage() const {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return sizeof(*this);
  }
  
  size_t baseUsage = sizeof(*this);
  size_t hidUsage = 0;
  
  if (pHidDevice) {
    hidUsage += 1024;  // Conservative estimate for HID device
  }
  
  return baseUsage + hidUsage;
#else
  return sizeof(*this);
#endif
}

bool BLEKeyboardHandler::isIdle() const {
#ifdef CONFIG_BT_ENABLED
  return initialized && (millis() - lastActivityTime > IDLE_TIMEOUT_MS);
#else
  return true;
#endif
}

#ifdef CONFIG_BT_ENABLED
bool BLEKeyboardHandler::setupHidDescriptor() {
  if (!pHidDevice) {
    return false;
  }
  
  // Create minimal HID report descriptor for keyboard
  static const uint8_t hidReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x95, 0x08,        //   Report Count (8)
    0x75, 0x01,        //   Report Size (1)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0                 // End Collection
  };
  
  pHidDevice->setReportMap(hidReportDescriptor, sizeof(hidReportDescriptor));
  return true;
}

int BLEKeyboardHandler::mapScancodeToButton(uint8_t scancode) const {
  // Map common keyboard scancodes to CrossPoint buttons
  // Optimized for e-reader usage
  
  switch (scancode) {
    // Navigation keys
    case 0x4C: // DELETE (mapped to Back)
    case 0xB2: return 0; // BACK button
      
    case 0x28: return 1; // RETURN (mapped to Confirm)
    
    case 0x50: return 2; // LEFT ARROW
    case 0x52: return 3; // UP ARROW  
    case 0x4F: return 4; // RIGHT ARROW
    case 0x51: return 5; // DOWN ARROW
      
    // Volume keys (side buttons)
    case 0x80: return 6; // VOLUME UP (mapped to Next page)
    case 0x81: return 7; // VOLUME DOWN (mapped to Prev page)
      
    // Space and Enter for page turning
    case 0x2C: return 6; // SPACE (Next page)
    case 0x28: return 7; // ENTER (Prev page) - conflict, prioritize Confirm
      
    // Number keys for quick access
    case 0x27: return 1; // ESC (can be mapped to Home)
    
    default:
      return -1; // Unmapped key
  }
}

void BLEKeyboardHandler::handleModifiers(uint8_t modifiers) {
  // Handle modifier keys (Shift, Ctrl, Alt, GUI)
  // Can be used for special functions
  
  if (modifiers & 0x02) { // Shift
    // Shift can modify button behavior
  }
  
  if (modifiers & 0x01) { // Ctrl
    // Ctrl can be used for shortcuts
  }
  
  if (modifiers & 0x04) { // Alt
    // Alt can be used for alternative functions
  }
}

void BLEKeyboardHandler::KeyboardCallbacks::onWrite(NimBLECharacteristic* pCharacteristic) {
  // Handle keyboard input data
  if (pCharacteristic && pCharacteristic->getLength() > 0) {
    BLE_KEYBOARD.processKeyboardReport(pCharacteristic->getData(), pCharacteristic->getLength());
  }
}

void BLEKeyboardHandler::KeyboardCallbacks::onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) {
  Serial.printf("[%lu] [KBD] Keyboard connected\n", millis());
  BLE_KEYBOARD.connected = true;
}

void BLEKeyboardHandler::KeyboardCallbacks::onUnsubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) {
  Serial.printf("[%lu] [KBD] Keyboard disconnected\n", millis());
  BLE_KEYBOARD.connected = false;
}
#endif