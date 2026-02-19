#include "RecoveryActivity.h"

#include "RecoveryUtils.h"
#include <GfxRenderer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

#include "fontIds.h"
#include "../../components/UITheme.h"

static const char* ITEMS[] = {
  "Flash firmware from SD card",
  "Reboot to normal mode",
};

void RecoveryActivity::onEnter() {
  Activity::onEnter();

  Recovery::getPartitions(appPartition, recoveryPartition);

  // Boot successful, mask to allow always booting back into recovery (fail-safe)
  esp_ota_mark_app_valid_cancel_rollback();

  requestUpdate();
}

void RecoveryActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // TODO
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    GUI.drawPopup(renderer, "Rebooting...");
    delay(200);  // Small delay to allow display to update before rebooting
    Recovery::reboot(false);
  }
}

void RecoveryActivity::render(RenderLock&&) {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = 100;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, top, "Recovery mode", true, EpdFontFamily::Style::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + height, "[1] Flash firmware from SD card", true, EpdFontFamily::Style::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * height, "[2] Reboot to normal mode", true, EpdFontFamily::Style::REGULAR);

  const auto labels = mappedInput.mapLabels("[1]", "[2]", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
