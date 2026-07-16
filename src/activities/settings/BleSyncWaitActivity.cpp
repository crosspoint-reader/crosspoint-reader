#include "BleSyncWaitActivity.h"

#include <Arduino.h>  // millis
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "activities/ActivityManager.h"  // activityManager, goToReaderDirect
#include "ble_sync/BleSyncIndicator.h"
#include "ble_sync/BleSyncManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BleSyncWaitActivity::BleSyncWaitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                         unsigned long deadlineMs)
    : Activity(tr(STR_BLE_SYNCING), renderer, mappedInput), bookPath_(std::move(bookPath)), deadlineMs_(deadlineMs) {}

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
                 tr(STR_BLE_SYNCING_WITH_PEER), nullptr);

  const BleSync::Status s = BLE_SYNC.status();
  char msg[40];
  switch (s.phase) {
    case BleSync::Phase::Advertising:
      std::snprintf(msg, sizeof(msg), "%s", tr(STR_BLE_CONNECTING_TO_PEER));
      break;
    case BleSync::Phase::Syncing:
      if (s.bookCount > 0)
        std::snprintf(msg, sizeof(msg), tr(STR_BLE_SYNCING_BOOKS_FORMAT), s.bookIndex, s.bookCount);
      else
        std::snprintf(msg, sizeof(msg), "%s", tr(STR_BLE_SYNCING));
      break;
    case BleSync::Phase::Success:
      std::snprintf(msg, sizeof(msg), "%s", tr(STR_BLE_SYNCED));
      break;
    case BleSync::Phase::Failed:
      std::snprintf(msg, sizeof(msg), "%s", tr(STR_SYNC_FAILED_MSG));
      break;
    default:
      std::snprintf(msg, sizeof(msg), "%s", tr(STR_OPENING));
      break;
  }

  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int cy = screen.y + screen.height / 3;
  renderer.drawCenteredText(UI_10_FONT_ID, cy, msg, true);
  renderer.drawCenteredText(UI_10_FONT_ID, cy + lh * 2, tr(STR_PRESS_ANY_KEY_TO_SKIP), true);

  const auto labels = mappedInput.mapLabels(tr(STR_SKIP), tr(STR_SKIP), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
