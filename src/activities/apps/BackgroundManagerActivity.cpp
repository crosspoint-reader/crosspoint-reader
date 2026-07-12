#include "BackgroundManagerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void BackgroundManagerActivity::refreshItems() {
  items.clear();

  // WiFi
  {
    BgItem item;
    item.name = "WiFi";
    if (WiFi.status() == WL_CONNECTED) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Connected: %s", WiFi.SSID().c_str());
      item.status = buf;
    } else {
      item.status = "Disconnected";
    }
    items.push_back(std::move(item));
  }

  // BLE
  {
    BgItem item;
    item.name = "BLE";
    item.status = (RADIO.getState() == RadioManager::RadioState::BLE) ? "Active" : "Available";
    items.push_back(std::move(item));
  }

  // SD Card
  {
    BgItem item;
    item.name = "SD Card";
    item.status = Storage.ready() ? "Mounted" : "Not available";
    items.push_back(std::move(item));
  }

  // Free Heap
  {
    BgItem item;
    item.name = "Free Heap";
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));
    item.status = buf;
    items.push_back(std::move(item));
  }

  // Uptime
  {
    BgItem item;
    item.name = "Uptime";
    int64_t uptimeUs = esp_timer_get_time();
    unsigned long uptimeSec = static_cast<unsigned long>(uptimeUs / 1000000LL);
    unsigned int h = uptimeSec / 3600;
    unsigned int m = (uptimeSec % 3600) / 60;
    unsigned int s = uptimeSec % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
    item.status = buf;
    items.push_back(std::move(item));
  }
}

void BackgroundManagerActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  refreshItems();
  requestUpdate();
}

void BackgroundManagerActivity::onExit() { Activity::onExit(); }

void BackgroundManagerActivity::loop() {
  const int count = static_cast<int>(items.size());

  buttonNavigator.onNext([this, count] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    refreshItems();
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void BackgroundManagerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Background Manager");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = static_cast<int>(items.size());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectorIndex,
      [this](int i) -> std::string { return items[i].name; },
      [this](int i) -> std::string { return items[i].status; });

  GUI.drawButtonHints(renderer, "Back", "Refresh", "Up", "Down");

  renderer.displayBuffer();
}
