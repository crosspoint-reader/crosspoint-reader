#include "TrmnlTestActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "trmnl/TrmnlService.h"

TrmnlTestActivity::TrmnlTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OnFinishCallback onFinish)
    : ActivityWithSubactivity("TrmnlTest", renderer, mappedInput), onFinish(std::move(onFinish)) {}

void TrmnlTestActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  LOG_DBG("TRMNL", "Turning on WiFi for test download...");
  WiFi.mode(WIFI_STA);

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
  } else {
    LOG_DBG("TRMNL", "Launching WifiSelectionActivity...");
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  }
}

void TrmnlTestActivity::onExit() {
  ActivityWithSubactivity::onExit();
}

void TrmnlTestActivity::onWifiSelectionComplete(bool success) {
  exitActivity();

  if (!success) {
    LOG_ERR("TRMNL", "WiFi connection failed");
    state = State::FAILURE;
    statusMessage = "WiFi Connection Failed";
    requestUpdate();
    return;
  }

  LOG_DBG("TRMNL", "WiFi connected, starting test download");
  state = State::DOWNLOADING;
  statusMessage = "Downloading...";
  requestUpdate();
  requestUpdateAndWait();

  if (TrmnlService::refreshScreen()) {
    LOG_DBG("TRMNL", "Download successful");
    state = State::SUCCESS;
  } else {
    LOG_ERR("TRMNL", "Download failed");
    state = State::FAILURE;
    statusMessage = "Download Failed";
  }
  requestUpdate();
}

void TrmnlTestActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onFinish();
  }
}

void TrmnlTestActivity::render(RenderLock&&) {
  if (subActivity) {
    return;
  }

  renderer.clearScreen();

  if (state == State::SUCCESS) {
    drawBmpFromFile(renderer, "/TRMNL/sleep.bmp", 0, 0);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Test Download", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::BOLD);

  if (state == State::FAILURE) {
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

// This function is copied from the original TrmnlSetupActivity.cpp
void TrmnlTestActivity::drawBmpFromFile(GfxRenderer& renderer, const char* filename, int x, int y) {
  FsFile file;
  if (!Storage.openFileForRead("TRMNL", filename, file)) {
    renderer.drawCenteredText(UI_10_FONT_ID, 100, "File not found");
    return;
  }

  uint8_t header[54];
  if (file.read(header, 54) != 54) {
    file.close();
    return;
  }

  if (header[0] != 'B' || header[1] != 'M') {
    file.close();
    return;
  }

  uint32_t dataOffset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
  int32_t width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
  int32_t height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
  uint16_t bpp = header[28] | (header[29] << 8);

  if (bpp != 1) {
    file.close();
    renderer.drawCenteredText(UI_10_FONT_ID, 100, "Unsupported BMP BPP");
    return;
  }

  bool flipY = true;
  if (height < 0) {
    height = -height;
    flipY = false;
  }

  int rowSize = (width * bpp + 31) / 32 * 4;
  auto* lineBuffer = new uint8_t[rowSize];

  file.seek(dataOffset);

  for (int row = 0; row < height; row++) {
    file.read(lineBuffer, rowSize);
    int drawY = y + (flipY ? (height - 1 - row) : row);

    for (int col = 0; col < width; col++) {
      int byteIdx = col / 8;
      int bitIdx = 7 - (col % 8);
      bool pixel = (lineBuffer[byteIdx] >> bitIdx) & 1;
      renderer.drawPixel(x + col, drawY, !pixel); // Invert pixel for display
    }
  }

  delete[] lineBuffer;
  file.close();
}