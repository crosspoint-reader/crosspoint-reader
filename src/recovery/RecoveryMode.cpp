#include "RecoveryMode.h"

#ifdef RECOVERY
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <builtinFonts/all.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "RecoveryUtils.h"
#include "fontIds.h"

HalDisplay display;
HalGPIO gpio;
GfxRenderer renderer(display);

EpdFont ui10RegularFont(&recovery_10_regular);
EpdFontFamily ui10FontFamily(&ui10RegularFont);

static const char* ITEMS[] = {
    "Flash firmware from SD card",
    "Reboot to normal mode",
};

void RecoveryMode::setup() {
  Recovery::getPartitions(appPartition, recoveryPartition);

  gpio.begin();
  powerManager.begin();

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 3 seconds for Serial to be ready to catch early logs
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) {
      delay(10);
    }
  }

  display.begin();
  renderer.begin();
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  LOG_INF("REC", "Fonts setup");

  // Boot successful, mask to allow always booting back into recovery (fail-safe)
  esp_ota_mark_app_valid_cancel_rollback();

  if (!Storage.begin()) {
    LOG_INF("REC", "SD card initialization failed");
    state = State::ERROR_SDCARD;
  } else {
    state = State::IDLE;
  }

  render();
  requestRender = false;
}

void RecoveryMode::loop() {
  gpio.update();

  if (state == State::ERROR_SDCARD) {
    // do nothing, just show the error message
    return;
  } else if (state == State::RESTART_TO_NORMAL) {
    LOG_INF("REC", "Rebooting to normal mode");
    delay(50);  // Small delay to allow display to update before rebooting
    Recovery::reboot(false);
    return;
  }

  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    state = State::FLASHING;
    requestRender = true;
  } else if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    state = State::RESTART_TO_NORMAL;
    requestRender = true;
  }

  if (requestRender) {
    render();
    requestRender = false;
  }
}

void RecoveryMode::render() {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = 100;
  const auto bottom = renderer.getScreenHeight() - 40;

  renderer.clearScreen();

  if (state == State::ERROR_SDCARD) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "SD card error", true);

  } else if (state == State::RESTART_TO_NORMAL) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Rebooting...", true);

  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Recovery mode", true);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height, "[1] Flash firmware from SD card", true);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * height, "[2] Reboot to normal mode", true);

    renderer.drawCenteredText(UI_10_FONT_ID, bottom, "[1]          [2]                    [3]          [4]", true);
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

#endif  // RECOVERY