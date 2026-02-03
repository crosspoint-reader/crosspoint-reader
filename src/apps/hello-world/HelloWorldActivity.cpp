#include "HelloWorldActivity.h"

#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <builtinFonts/all.h>

#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "fontIds.h"

RTC_DATA_ATTR int boot_count = 0;

namespace {
EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);
}

HelloWorldActivity::HelloWorldActivity(HalDisplay& display, HalGPIO& input)
    : display_(display), input_(input), needsUpdate_(true) {}

void HelloWorldActivity::onEnter() {
  display_.begin();

  boot_count++;
  if (boot_count > 3) {
    returnToLauncher();
    return;
  }

  needsUpdate_ = true;
}

void HelloWorldActivity::loop() {
  if (input_.wasPressed(HalGPIO::BTN_BACK)) {
    returnToLauncher();
    return;
  }

  if (needsUpdate_) {
    render();
    needsUpdate_ = false;
  }
}

void HelloWorldActivity::onExit() {}

void HelloWorldActivity::render() {
  GfxRenderer renderer(display_);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.clearScreen();

  const int pageHeight = renderer.getScreenHeight();
  const int y = (pageHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Hello World!");
  renderer.displayBuffer();
}

void HelloWorldActivity::returnToLauncher() {
  boot_count = 0;
  esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
  esp_restart();
}
