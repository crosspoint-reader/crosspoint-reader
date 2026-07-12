#include "AirTagTestActivity.h"

#include <BLEDevice.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

constexpr const char* AirTagTestActivity::MODE_NAMES[];

void AirTagTestActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureBle();

  state = MODE_SELECT;
  modeIndex = 0;
  advCount = 0;
  startTime = 0;
  lastRotateTime = 0;
  lastAdvTime = 0;
  pAdvertising = nullptr;
  memset(currentMac, 0, sizeof(currentMac));

  requestUpdate();
}

void AirTagTestActivity::onExit() {
  Activity::onExit();
  stopSpoofing();
  RADIO.shutdown();
}

void AirTagTestActivity::generateRandomMac() {
  for (int i = 0; i < 6; i++) {
    currentMac[i] = esp_random() & 0xFF;
  }
  // Set top two bits for random static address
  currentMac[0] |= 0xC0;
  // Random MAC is embedded in advertisement payload, not GAP address
}

std::string AirTagTestActivity::formatMac() const {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", currentMac[0], currentMac[1], currentMac[2],
           currentMac[3], currentMac[4], currentMac[5]);
  return std::string(buf);
}

void AirTagTestActivity::startSpoofing(MacMode mode) {
  macMode = mode;
  state = SPOOFING;
  advCount = 0;
  startTime = millis();
  lastRotateTime = millis();
  lastAdvTime = 0;

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->stop();

  generateRandomMac();
  requestUpdate();
}

void AirTagTestActivity::stopSpoofing() {
  if (pAdvertising) {
    pAdvertising->stop();
    pAdvertising = nullptr;
  }
  state = MODE_SELECT;
}

void AirTagTestActivity::sendAirTagAdvertisement() {
  if (!pAdvertising) return;

  pAdvertising->stop();

  BLEAdvertisementData advData;

  // Apple Find My network payload
  // Company ID 0x004C (Apple) + Find My type/length + public key payload
  uint8_t findMyPayload[] = {
      0x12, 0x19,  // Find My type and length
      0x10,        // Status byte
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00,  // First byte of public key bits
      0x00,  // Hint
  };

  // Fill simulated public key bytes with random data
  for (int i = 3; i < 25; i++) {
    findMyPayload[i] = esp_random() & 0xFF;
  }

  std::string mfData;
  // Apple company ID (little-endian)
  mfData += (char)0x4C;
  mfData += (char)0x00;
  mfData.append(reinterpret_cast<char*>(findMyPayload), sizeof(findMyPayload));

  advData.setManufacturerData(String(mfData.data(), mfData.size()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  advCount++;
}

void AirTagTestActivity::loop() {
  if (state == MODE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startSpoofing(static_cast<MacMode>(modeIndex));
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

  // SPOOFING state
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    stopSpoofing();
    requestUpdate();
    return;
  }

  unsigned long now = millis();

  // Rotate MAC if in rotating mode
  if (macMode == ROTATING_MAC && (now - lastRotateTime >= ROTATE_INTERVAL_MS)) {
    lastRotateTime = now;
    generateRandomMac();
    LOG_DBG("AirTag", "Rotated MAC to %s", formatMac().c_str());
  }

  // Send advertisement periodically
  if (now - lastAdvTime >= ADV_INTERVAL_MS) {
    lastAdvTime = now;
    sendAirTagAdvertisement();
    requestUpdate();
  }
}

void AirTagTestActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "AirTag Spoofer");

  if (state == MODE_SELECT) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, MODE_COUNT, modeIndex,
        [](int index) { return std::string(MODE_NAMES[index]); });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // SPOOFING display
    const int centerY = pageHeight / 2 - 50;

    const char* modeName = (macMode == STATIC_MAC) ? "Static MAC" : "Rotating MAC";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, modeName, true, EpdFontFamily::BOLD);

    // Current MAC address
    std::string macStr = "MAC: " + formatMac();
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 30, macStr.c_str());

    // Advertisement count
    char buf[64];
    snprintf(buf, sizeof(buf), "Advertisements: %lu", (unsigned long)advCount);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 55, buf);

    // Uptime
    unsigned long elapsed = (millis() - startTime) / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %lum %lus", elapsed / 60, elapsed % 60);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 80, buf);

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
