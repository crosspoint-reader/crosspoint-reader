#include "BleSyncWaitActivity.h"

#include <Arduino.h>  // millis
#include <GfxRenderer.h>

#include <cstdio>

#include "activities/ActivityManager.h"  // activityManager, goToReaderDirect
#include "ble_sync/BleSyncIndicator.h"
#include "ble_sync/BleSyncManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BleSyncWaitActivity::BleSyncWaitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string bookPath, unsigned long deadlineMs)
    : Activity("Syncing", renderer, mappedInput), bookPath_(std::move(bookPath)), deadlineMs_(deadlineMs) {}

void BleSyncWaitActivity::onEnter() {
  Activity::onEnter();
  BLE_SYNC.start(renderer, deadlineMs_, /*blocking=*/true);
  requestUpdate();
}

void BleSyncWaitActivity::openBook() {
  if (opened_) return;
  opened_ = true;
  activityManager.goToReaderDirect(bookPath_);  // bypass the gate — no re-entry
}

void BleSyncWaitActivity::loop() {
  if (opened_) return;

  // Skip: any key opens the book now at the local position (the one place a sync
  // is skippable — PROTOCOL-v2.md §5).
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    BLE_SYNC.stop();
    openBook();
    return;
  }

  // Reconcile finished (or never had a phone) → open the book.
  if (!BLE_SYNC.isActive()) {
    openBook();
    return;
  }
}

void BleSyncWaitActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, metrics.topPadding, screen.width, metrics.headerHeight},
                 "Syncing with phone", nullptr);

  const BleSync::Status s = BLE_SYNC.status();
  char msg[40];
  switch (s.phase) {
    case BleSync::Phase::Advertising:
      std::snprintf(msg, sizeof(msg), "Connecting to phone\xE2\x80\xA6");
      break;
    case BleSync::Phase::Syncing:
      if (s.bookCount > 0)
        std::snprintf(msg, sizeof(msg), "Syncing %d of %d", s.bookIndex, s.bookCount);
      else
        std::snprintf(msg, sizeof(msg), "Syncing\xE2\x80\xA6");
      break;
    case BleSync::Phase::Success:
      std::snprintf(msg, sizeof(msg), "Synced");
      break;
    case BleSync::Phase::Failed:
      std::snprintf(msg, sizeof(msg), "Sync failed");
      break;
    default:
      std::snprintf(msg, sizeof(msg), "Opening\xE2\x80\xA6");
      break;
  }

  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int cy = screen.y + screen.height / 3;
  renderer.drawCenteredText(UI_10_FONT_ID, cy, msg, true);
  renderer.drawCenteredText(UI_10_FONT_ID, cy + lh * 2, "Press any key to skip", true);

  const auto labels = mappedInput.mapLabels("Skip", "Skip", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
