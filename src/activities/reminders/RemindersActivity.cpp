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
  requestUpdate();
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
      // Manual re-sync on a double-tap of Confirm (single press is ignored to
      // avoid accidental syncs).
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
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

      // Down: next page (if the list overflowed). Up: previous page.
      if (mappedInput.wasPressed(MappedInputManager::Button::Down) && lastNextIndex < gRemindersData.count) {
        if (pageDepth < REMINDERS_MAX_ITEMS) pageHistory[pageDepth++] = pageStart;
        pageStart = lastNextIndex;
        tickRefresh = false;
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up) && pageDepth > 0) {
        pageStart = pageHistory[--pageDepth];
        tickRefresh = false;
        requestUpdate();
        return;
      }

      if (!staleReached && millis() - showStartMs >= TICK_MS * static_cast<unsigned long>(tickCount + 1)) {
        tickCount++;
        onMinuteTick();
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
  requestUpdate();  // renderFull() draws the stale banner because is_stale is set
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
        const bool ok = RemindersRenderer::renderCountdownsOnly(renderer, gRemindersData, pageStart);
        tickRefresh = false;
        if (!ok) {
          // An item elapsed: clear ghosting with a full refresh.
          lastNextIndex = RemindersRenderer::renderFull(renderer, gRemindersData, pageStart);
        }
      } else {
        lastNextIndex = RemindersRenderer::renderFull(renderer, gRemindersData, pageStart);
      }
      return;
    }
    case State::Failed: {
      const int midY = renderer.getScreenHeight() / 2;
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_REMINDERS_SYNC_FAILED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_REMINDERS_SYNC_FAILED_HINT));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REMINDERS_RETRY), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }
  }
}
