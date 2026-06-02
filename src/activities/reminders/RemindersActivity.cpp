#include "RemindersActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "RemindersRenderer.h"
#include "RemindersState.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RemindersActivity::onEnter() {
  Activity::onEnter();
  // Force portrait: we may arrive from a landscape reader, and the Syncing /
  // Failed screens (and the renderer) assume the 480x800 portrait panel.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  state = State::Syncing;
  syncStarted = false;
  syncDone = false;
  syncCancel = false;
  staleReached = false;
  tickRefresh = false;
  tickCount = 0;
  pageStart = 0;
  pageDepth = 0;
  lastNextIndex = 0;
  selectedIndex = -1;
  requestUpdate();
}

void RemindersActivity::onExit() {
  // If a sync task is somehow still running, tear it down. This is best-effort:
  // preventAutoSleep() keeps us alive during the sync, so this normally only
  // fires after the task has already self-deleted.
  if (syncTask != nullptr && !syncDone) {
    vTaskDelete(syncTask);
    syncTask = nullptr;
  }
  // Persist so SleepActivity's Reminders mode (and a cold boot) can redraw the
  // last-known list without a network round-trip.
  gRemindersData.saveToFile();
  Activity::onExit();
}

void RemindersActivity::syncTaskTrampoline(void* param) { static_cast<RemindersActivity*>(param)->runSyncTask(); }

void RemindersActivity::runSyncTask() {
  // Blocking: WiFi connect + NTP + two HTTPS calls. Runs off the main loop so a
  // ~15s sync can't trip the loop task. Writes straight into gRemindersData on
  // success (render() only reads gRemindersData once state == Showing). syncCancel
  // is polled by GoogleClient at phase/IO boundaries so a Back press aborts promptly.
  syncResult = GoogleClient::syncAll(gRemindersData, &syncCancel);
  LOG_DBG("RMND", "Sync task stack high-water: %u bytes", uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
  syncDone = true;
  TaskHandle_t self = syncTask;
  syncTask = nullptr;
  vTaskDelete(self);  // never returns
}

void RemindersActivity::startSync() {
  state = State::Syncing;
  syncStarted = false;
  syncDone = false;
  syncCancel = false;
  staleReached = false;
  tickRefresh = false;
  tickCount = 0;
  pageStart = 0;
  pageDepth = 0;
  lastNextIndex = 0;
  selectedIndex = -1;
  requestUpdate();
}

void RemindersActivity::completeTaskTrampoline(void* param) {
  static_cast<RemindersActivity*>(param)->runCompleteTask();
}

void RemindersActivity::runCompleteTask() {
  syncResult = GoogleClient::markTaskComplete(completeItemIndex, gRemindersData, &syncCancel);
  LOG_DBG("RMND", "Complete task stack high-water: %u bytes",
          uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
  syncDone = true;
  TaskHandle_t self = syncTask;
  syncTask = nullptr;
  vTaskDelete(self);
}

void RemindersActivity::startComplete(uint8_t itemIndex) {
  completeItemIndex = itemIndex;
  selectedIndex = -1;
  state = State::Completing;
  syncDone = false;
  syncCancel = false;
  syncResult = GoogleClient::Result::OK;
  requestUpdateAndWait();
  if (xTaskCreate(&completeTaskTrampoline, "RmndComplete", SYNC_TASK_STACK, this, 1, &syncTask) != pdPASS) {
    LOG_ERR("RMND", "Failed to create complete task");
    syncTask = nullptr;
    syncResult = GoogleClient::Result::FetchFailed;
    syncDone = true;
  }
}

void RemindersActivity::loop() {
  switch (state) {
    case State::Syncing: {
      if (!syncStarted) {
        // Paint the SYNCING screen before the (off-loop) sync begins.
        requestUpdateAndWait();
        syncStarted = true;
        if (xTaskCreate(&syncTaskTrampoline, "RmndSync", SYNC_TASK_STACK, this, 1, &syncTask) != pdPASS) {
          LOG_ERR("RMND", "Failed to create sync task");
          syncTask = nullptr;
          syncResult = GoogleClient::Result::FetchFailed;
          syncDone = true;
        }
        return;
      }
      // Back aborts an in-flight sync; GoogleClient polls syncCancel and returns
      // Cancelled, after which we exit cleanly (no mid-TLS task kill).
      if (!syncCancel && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        LOG_DBG("RMND", "Sync cancel requested");
        syncCancel = true;
      }
      if (syncDone) {
        if (syncResult == GoogleClient::Result::Cancelled) {
          finish();
          return;
        }
        // A garbage clock makes every countdown meaningless, so never fall back
        // to the cached list for it — surface the failure instead.
        const bool usable = syncResult != GoogleClient::Result::ClockUnset &&
                            (syncResult == GoogleClient::Result::OK || gRemindersData.count > 0);
        if (usable) {
          // Fresh sync, or a usable cached list to fall back on.
          if (syncResult != GoogleClient::Result::OK) {
            LOG_INF("RMND", "Sync %s; showing cached list", GoogleClient::resultName(syncResult));
            gRemindersData.is_stale = true;
          }
          state = State::Showing;
          showStartMs = millis();
          tickCount = 0;
          tickRefresh = false;
          staleReached = gRemindersData.is_stale;
        } else {
          LOG_ERR("RMND", "Sync failed: %s", GoogleClient::resultName(syncResult));
          state = State::Failed;
        }
        requestUpdate();
      }
      return;
    }

    case State::Showing: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        // Single Confirm on a selected completable task: mark it complete.
        if (selectedIndex >= 0) {
          LOG_DBG("RMND", "Confirm: completing task index %d", selectedIndex);
          startComplete(static_cast<uint8_t>(selectedIndex));
          return;
        }
        // No selection: double-tap Confirm triggers a manual re-sync.
        const unsigned long nowMs = millis();
        if (nowMs - lastConfirmMs <= DOUBLE_TAP_MS) {
          lastConfirmMs = 0;
          LOG_DBG("RMND", "Double-tap Confirm: re-syncing");
          startSync();
          return;
        }
        lastConfirmMs = nowMs;
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
        return;
      }

      // Down: next page. Up: previous page. Both reset the task selection.
      if (mappedInput.wasPressed(MappedInputManager::Button::Down) && lastNextIndex < gRemindersData.count) {
        if (pageDepth < REMINDERS_MAX_ITEMS) pageHistory[pageDepth++] = pageStart;
        pageStart = lastNextIndex;
        selectedIndex = -1;
        tickRefresh = false;
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up) && pageDepth > 0) {
        pageStart = pageHistory[--pageDepth];
        selectedIndex = -1;
        tickRefresh = false;
        requestUpdate();
        return;
      }

      // Right/Left: cycle the task-completion cursor through completable items
      // on the current page (tasks with a non-empty task_id).
      const uint8_t pageEnd = lastNextIndex;
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        int8_t next = -1;
        for (uint8_t k = (selectedIndex >= 0 ? selectedIndex + 1 : pageStart); k < pageEnd; k++) {
          const CalItem& c = gRemindersData.items[k];
          if (!c.is_calendar && c.task_id[0] != '\0') {
            next = static_cast<int8_t>(k);
            break;
          }
        }
        if (next != selectedIndex) {
          selectedIndex = next;
          tickRefresh = false;
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        int8_t prev = -1;
        const uint8_t scanEnd = selectedIndex >= 0 ? static_cast<uint8_t>(selectedIndex) : pageEnd;
        for (int k = static_cast<int>(scanEnd) - 1; k >= static_cast<int>(pageStart); k--) {
          const CalItem& c = gRemindersData.items[k];
          if (!c.is_calendar && c.task_id[0] != '\0') {
            prev = static_cast<int8_t>(k);
            break;
          }
        }
        if (prev != selectedIndex) {
          selectedIndex = prev;
          tickRefresh = false;
          requestUpdate();
        }
        return;
      }

      if (!staleReached && millis() - showStartMs >= TICK_MS * static_cast<unsigned long>(tickCount + 1)) {
        tickCount++;
        onMinuteTick();
      }
      return;
    }

    case State::Completing: {
      if (syncDone) {
        if (syncResult != GoogleClient::Result::OK) {
          LOG_ERR("RMND", "Task completion failed: %s", GoogleClient::resultName(syncResult));
        }
        state = State::Showing;
        tickRefresh = false;
        requestUpdate();
      }
      return;
    }

    case State::Failed: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        startSync();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
      }
      return;
    }
  }
}

void RemindersActivity::onMinuteTick() {
  if (tickCount >= LIVE_WINDOW_TICKS) {
    enterStale();
    return;
  }
  // Live countdown update via FAST_REFRESH.
  tickRefresh = true;
  requestUpdate();
}

void RemindersActivity::enterStale() {
  LOG_DBG("RMND", "Live window elapsed; entering stale state");
  gRemindersData.is_stale = true;
  staleReached = true;  // stops preventAutoSleep(); main loop will sleep on timeout
  tickRefresh = false;
  gRemindersData.saveToFile();
  requestUpdate();  // renderFull() suppresses the live countdown because is_stale is set
}

void RemindersActivity::render(RenderLock&&) {
  switch (state) {
    case State::Syncing: {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_REMINDERS_SYNCING), true,
                                EpdFontFamily::BOLD);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
    case State::Showing: {
      if (tickRefresh) {
        const bool ok = RemindersRenderer::renderCountdownsOnly(renderer, gRemindersData, pageStart, selectedIndex);
        tickRefresh = false;
        if (!ok) {
          // A full redraw resolves the default highlight; capture it so Confirm
          // and Left/Right operate on the auto-selected task.
          int8_t resolved = selectedIndex;
          lastNextIndex = RemindersRenderer::renderFull(renderer, gRemindersData, pageStart, selectedIndex,
                                                        /*autoSelectFirst=*/true, &resolved);
          selectedIndex = resolved;
        }
      } else {
        // Auto-highlight the first completable task on the page so a single
        // Confirm checks it; capture the resolved index for input handling.
        int8_t resolved = selectedIndex;
        lastNextIndex = RemindersRenderer::renderFull(renderer, gRemindersData, pageStart, selectedIndex,
                                                      /*autoSelectFirst=*/true, &resolved);
        selectedIndex = resolved;
      }
      return;
    }
    case State::Completing: {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_REMINDERS_COMPLETING), true,
                                EpdFontFamily::BOLD);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
    case State::Failed: {
      const int midY = renderer.getScreenHeight() / 2;
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_REMINDERS_SYNC_FAILED), true, EpdFontFamily::BOLD);
      // Name the stage that actually failed so a single test run pins the cause
      // (Cancelled/OK never reach this screen — see loop()).
      StrId hint;
      switch (syncResult) {
        case GoogleClient::Result::NoCredentials:
          hint = StrId::STR_REMINDERS_FAIL_CREDS;
          break;
        case GoogleClient::Result::WifiFailed:
          hint = StrId::STR_REMINDERS_FAIL_WIFI;
          break;
        case GoogleClient::Result::ClockUnset:
          hint = StrId::STR_REMINDERS_FAIL_CLOCK;
          break;
        case GoogleClient::Result::AuthFailed:
          hint = StrId::STR_REMINDERS_FAIL_AUTH;
          break;
        default:
          hint = StrId::STR_REMINDERS_FAIL_FETCH;
          break;
      }
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, I18N.get(hint));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REMINDERS_RETRY), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
  }
}
