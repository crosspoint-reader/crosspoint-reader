#include "AboutActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <esp_mac.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_DEVICE = 0,
  ITEM_FIRMWARE,
  ITEM_CHIP,
  ITEM_FLASH,
  ITEM_DISPLAY,
  ITEM_RESOLUTION,
  ITEM_TOUCH,
  ITEM_FRONTLIGHT,
  ITEM_RTC,
  ITEM_IMU,
  ITEM_MAC,
};

// Deliberately hardcoded English, exempt from the tr() rule: support reads
// these screenshots across every device language, so the labels must be
// identical on every unit.
const char* const menuNames[AboutActivity::ITEM_COUNT] = {
    "Device", "Firmware",          "Chip",        "Flash", "Display Controller", "Resolution", "Touch", "Frontlight",
    "RTC",    "Tilt Sensor (IMU)", "MAC Address",
};

// Chip part numbers, not user prose — deliberately untranslated.
const char* displayControllerName(const BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
  }
  return "?";
}

const char* touchControllerName(const BoardConfig::TouchController c) {
  switch (c) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
  }
  return nullptr;
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("About", renderer, mappedInput) {}

const char* AboutActivity::headerTitle() const { return "About"; }

void AboutActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = menuNames[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  char buf[32];
  // Hardware and firmware information is fixed after boot; fill it once here.
  // BoardConfig::ACTIVE reflects RUNTIME detection: selectDevice() picked the
  // profile and applyXteinkDisplayController() may have promoted the display
  // controller to the panel actually found on the bus.
  rowValues_[ITEM_DEVICE] = BoardConfig::ACTIVE.name;
  rowValues_[ITEM_FIRMWARE] = CROSSPOINT_VERSION;
  snprintf(buf, sizeof(buf), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  rowValues_[ITEM_CHIP] = buf;
  snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  rowValues_[ITEM_FLASH] = buf;
  rowValues_[ITEM_DISPLAY] = displayControllerName(BoardConfig::ACTIVE.displayController);
  snprintf(buf, sizeof(buf), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  rowValues_[ITEM_RESOLUTION] = buf;
  const char* touch = touchControllerName(BoardConfig::ACTIVE.touch.controller);
  rowValues_[ITEM_TOUCH] = touch ? touch : "No";
  rowValues_[ITEM_FRONTLIGHT] = Frontlight.present() ? "Yes" : "No";
  rowValues_[ITEM_RTC] = halClock.isAvailable() ? "Yes" : "No";
  rowValues_[ITEM_IMU] = halTiltSensor.isAvailable() ? "Yes" : "No";
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  rowValues_[ITEM_MAC] = buf;
}

void AboutActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
