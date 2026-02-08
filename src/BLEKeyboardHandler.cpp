#include "BLEKeyboardHandler.h"

#ifdef ARDUINO
#include "Arduino.h"
#include "MappedInputManager.h"
#endif

bool BLEKeyboardHandler::initialize(NimBLEServer* server) {
#ifdef CONFIG_BT_ENABLED
  if (initialized || !server) {
    return initialized;
  }

  Serial.printf("[%lu] [KBD] Initializing BLE Keyboard\n", millis());

  try {
    pServer = server;

    // Create HID device
    pHidDevice = new NimBLEHIDDevice(pServer);
    if (!pHidDevice) {
      Serial.printf("[%lu] [KBD] Failed to create HID device\n", millis());
      return false;
    }

    // Setup HID report descriptor
    if (!setupHidDescriptor()) {
      Serial.printf("[%lu] [KBD] Failed to setup HID descriptor\n", millis());
      delete pHidDevice;
      pHidDevice = nullptr;
      return false;
    }

    // Create custom keyboard service for direct characteristic writes
    pService = pServer->createService("12345678-1234-1234-1234-123456789abc");
    if (!pService) {
      Serial.printf("[%lu] [KBD] Failed to create service\n", millis());
      delete pHidDevice;
      pHidDevice = nullptr;
      return false;
    }

    // Create input characteristic
    pInputCharacteristic = pService->createCharacteristic(
        "87654321-4321-4321-4321-cba987654321", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    if (!pInputCharacteristic) {
      Serial.printf("[%lu] [KBD] Failed to create input characteristic\n", millis());
      delete pHidDevice;
      pHidDevice = nullptr;
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
  (void)server;
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
  pService = nullptr;
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
  // data[0] = modifiers, data[1] = reserved, data[2..7] = keycodes
  uint8_t keycodes[6] = {data[2], data[3], data[4], data[5], data[6], data[7]};

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
#else
  (void)data;
  (void)length;
#endif
}

void BLEKeyboardHandler::update() {
#ifdef CONFIG_BT_ENABLED
  if (!initialized) {
    return;
  }

  // Check for extended idle timeout
  if (connected && isIdle() && (millis() - lastActivityTime > IDLE_TIMEOUT_MS * 2)) {
    Serial.printf("[%lu] [KBD] Very long idle, considering disconnect\n", millis());
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
  static uint8_t hidReportDescriptor[] = {
      0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
      0x09, 0x06,  // Usage (Keyboard)
      0xA1, 0x01,  // Collection (Application)
      0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
      0x19, 0xE0,  //   Usage Minimum (224)
      0x29, 0xE7,  //   Usage Maximum (231)
      0x15, 0x00,  //   Logical Minimum (0)
      0x25, 0x01,  //   Logical Maximum (1)
      0x95, 0x08,  //   Report Count (8)
      0x75, 0x01,  //   Report Size (1)
      0x81, 0x02,  //   Input (Data,Var,Abs)
      0x95, 0x01,  //   Report Count (1)
      0x75, 0x08,  //   Report Size (8)
      0x81, 0x03,  //   Input (Const,Var,Abs)
      0x95, 0x06,  //   Report Count (6)
      0x75, 0x08,  //   Report Size (8)
      0x15, 0x00,  //   Logical Minimum (0)
      0x25, 0x65,  //   Logical Maximum (101)
      0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
      0x19, 0x00,  //   Usage Minimum (0)
      0x29, 0x65,  //   Usage Maximum (101)
      0x81, 0x00,  //   Input (Data,Array,Abs)
      0xC0         // End Collection
  };

  pHidDevice->setReportMap(hidReportDescriptor, sizeof(hidReportDescriptor));
  return true;
}

int BLEKeyboardHandler::mapScancodeToButton(uint8_t scancode) const {
  // Map common keyboard scancodes to CrossPoint MappedInputManager::Button values
  // Button enum: Back=0, Confirm=1, Left=2, Right=3, Up=4, Down=5, Power=6, PageBack=7, PageForward=8
  switch (scancode) {
    // Navigation keys
    case 0x29:  // ESCAPE -> Back
    case 0x4C:  // DELETE -> Back
      return 0;  // Back

    case 0x28:  // RETURN -> Confirm
      return 1;  // Confirm

    case 0x50:  // LEFT ARROW
      return 2;  // Left

    case 0x4F:  // RIGHT ARROW
      return 3;  // Right

    case 0x52:  // UP ARROW
      return 4;  // Up

    case 0x51:  // DOWN ARROW
      return 5;  // Down

    // Page turning keys
    case 0x2C:  // SPACE -> Page Forward
      return 8;  // PageForward

    case 0x2A:  // BACKSPACE -> Page Back
      return 7;  // PageBack

    default:
      return -1;  // Unmapped key
  }
}

void BLEKeyboardHandler::KeyboardCallbacks::onWrite(NimBLECharacteristic* pCharacteristic,
                                                     NimBLEConnInfo& connInfo) {
  (void)connInfo;
  if (pCharacteristic && pCharacteristic->getLength() > 0) {
    extern BLEKeyboardHandler* getActiveKeyboardHandler();
    auto* handler = getActiveKeyboardHandler();
    if (handler) {
      auto value = pCharacteristic->getValue();
      handler->processKeyboardReport(value.data(), value.size());
    }
  }
}

void BLEKeyboardHandler::KeyboardCallbacks::onSubscribe(NimBLECharacteristic* pCharacteristic,
                                                         NimBLEConnInfo& connInfo, uint16_t subValue) {
  (void)pCharacteristic;
  (void)connInfo;
  (void)subValue;
  Serial.printf("[%lu] [KBD] Keyboard client subscribed\n", millis());
}
#endif
