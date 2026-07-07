#include "BootAlternateFirmwareActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaBootSwitch.h"

namespace {
bool isCrossPointFirmware(const char* projectName) {
  const char* prefix = "crosspoint";
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    if (projectName[i] == '\0' || std::tolower(static_cast<unsigned char>(projectName[i])) != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Draw a block of wrapped centered lines starting at y, return new y after last line.
int drawWrappedCentered(GfxRenderer& renderer, int fontId, int y, int lineH, const char* text, int maxWidth,
                        int maxLines, bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  for (const auto& line : lines) {
    renderer.drawCenteredText(fontId, y, line.c_str(), black, style);
    y += lineH;
  }
  return y;
}
}  // namespace

void BootAlternateFirmwareActivity::onEnter() {
  Activity::onEnter();

  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (other && esp_ota_get_partition_description(other, &altDesc) == ESP_OK) {
    altIsCrossPoint = isCrossPointFirmware(altDesc.project_name);
  } else {
    LOG_ERR("BOOT_ALT", "alternate partition not found or invalid on enter");
    state = State::FAILED;
  }

  requestUpdate();
}

void BootAlternateFirmwareActivity::onExit() { Activity::onExit(); }

void BootAlternateFirmwareActivity::doSwitch() {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) {
    LOG_ERR("BOOT_ALT", "no alternate partition");
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("BOOT_ALT", "switching to %s (crosspoint=%d)", other->label, altIsCrossPoint ? 1 : 0);
  if (!ota_boot::switchTo(other)) {
    LOG_ERR("BOOT_ALT", "switchTo failed");
    state = State::FAILED;
    requestUpdate();
    return;
  }

  delay(200);
  ESP.restart();
}

void BootAlternateFirmwareActivity::loop() {
  if (state == State::CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_INF("BOOT_ALT", "user confirmed switch");
      {
        RenderLock lock(*this);
        state = State::SWITCHING;
      }
      requestUpdateAndWait();
      doSwitch();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == State::FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void BootAlternateFirmwareActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int textMaxWidth = pageWidth - 2 * metrics.contentSidePadding;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SWITCH_FIRMWARE_TITLE));

  const auto lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight;
  const int midY = contentTop + (contentBottom - contentTop) / 2;

  if (state == State::CONFIRM) {
    // Lay out from midY upward: label (2 lines name + 1 version) = 3 lines above mid
    int y = midY - lineH * 3;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SWITCH_FIRMWARE_FOUND));
    y += lineH;
    y = drawWrappedCentered(renderer, UI_10_FONT_ID, y, lineH, altDesc.project_name, textMaxWidth, 2,
                            /*black=*/true, EpdFontFamily::BOLD);
    y = drawWrappedCentered(renderer, UI_10_FONT_ID, y, lineH, altDesc.version, textMaxWidth, 1);
    y += lineH / 2;

    if (altIsCrossPoint) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SWITCH_FIRMWARE_CONFIRM));
      y += lineH;
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SWITCH_FIRMWARE_RETURN_HINT));
    } else {
      drawWrappedCentered(renderer, UI_10_FONT_ID, y, lineH, tr(STR_SWITCH_FIRMWARE_UNKNOWN_WARNING), textMaxWidth, 3,
                          /*black=*/true, EpdFontFamily::BOLD);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SWITCH_FIRMWARE_BTN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::SWITCHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_SWITCH_FIRMWARE_SWITCHING));
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - lineH / 2, tr(STR_SWITCH_FIRMWARE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + lineH / 2, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
