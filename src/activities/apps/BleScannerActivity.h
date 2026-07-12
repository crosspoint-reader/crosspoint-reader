#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <BLEDevice.h>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BleScannerActivity final : public Activity {
 public:
  explicit BleScannerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleScanner", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return scanning; }

 private:
  enum State { SCANNING_VIEW, DETAIL, CONNECTING, SERVICES, CHARACTERISTICS, CHAR_VALUE };

  struct BleDevice {
    std::string name;
    std::string mac;
    int rssi;
    BLEAddress address{""};
  };

  State state = SCANNING_VIEW;
  std::vector<BleDevice> devices;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int detailIndex = 0;
  bool scanning = false;
  bool scanInitialized = false;
  unsigned long lastScanTime = 0;
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;
  static constexpr unsigned long SCAN_INTERVAL_MS = 5000;
  static constexpr int MAX_DEVICES = 50;

  // BLE Explorer (service/characteristic browsing)
  BLEClient* pClient = nullptr;
  bool connected = false;

  struct ServiceInfo {
    std::string uuid;
    std::string name;
    int charCount;
  };
  std::vector<ServiceInfo> services;

  struct CharInfo {
    std::string uuid;
    std::string name;
    std::string value;
    uint8_t properties;
    bool canRead;
    bool canWrite;
    bool canNotify;
  };
  std::vector<CharInfo> characteristics;

  int selectedDevice = -1;
  int selectedService = -1;
  int selectedChar = -1;
  bool needsInit = false;

  void startBleScan();
  void stopBleScan();
  void saveToCsv();

  void connectToDevice(int index);
  void disconnect();
  void enumerateServices();
  void enumerateCharacteristics(int serviceIndex);
  void readCharacteristic(int charIndex);

  static const char* resolveServiceName(const std::string& uuid);
  static const char* resolveCharName(const std::string& uuid);
  static std::string propertiesToString(uint8_t props);
  static std::string bytesToHex(const uint8_t* data, int len);
  static std::string bytesToAsciiSafe(const uint8_t* data, int len);
};
