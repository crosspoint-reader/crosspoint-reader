#include "RecoveryMode.h"

#ifdef RECOVERY
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "RecoveryUtils.h"
#include "fontIds.h"

#include <Logging.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <builtinFonts/all.h>

HalDisplay display;
HalGPIO gpio;
GfxRenderer renderer(display);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

static const char* ITEMS[] = {
    "Flash firmware from SD card",
    "Reboot to normal mode",
};

void RecoveryMode::setup() {
  Recovery::getPartitions(appPartition, recoveryPartition);

  // Boot successful, mask to allow always booting back into recovery (fail-safe)
  esp_ota_mark_app_valid_cancel_rollback();

  gpio.begin();
  display.begin();

  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  LOG_INF("REC", "Fonts setup");
  
  if (!Storage.begin()) {
    LOG_INF("REC", "SD card initialization failed");
    state = State::ERROR_SDCARD;
  } else {
    state = State::IDLE;
  }

  render();
}

void RecoveryMode::loop() {
  gpio.update();

  if (state == State::ERROR_SDCARD) {
    // do nothing, just show the error message
    return;
  }

  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    // TODO
    requestRender = true;
  }

  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    // GUI.drawPopup(renderer, "Rebooting...");
    requestRender = true;
    delay(200);  // Small delay to allow display to update before rebooting
    Recovery::reboot(false);
  }
  
  if (requestRender) {
    render();
    requestRender = false;
  }
}

void RecoveryMode::render() {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = 100;

  renderer.clearScreen();

  if (state == State::ERROR_SDCARD) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "SD card error", true, EpdFontFamily::Style::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Recovery mode", true, EpdFontFamily::Style::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height, "[1] Flash firmware from SD card", true,
                              EpdFontFamily::Style::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * height, "[2] Reboot to normal mode", true,
                              EpdFontFamily::Style::REGULAR);

    // const auto labels = mappedInput.mapLabels("[1]", "[2]", "", "");
    // GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

#endif  // RECOVERY