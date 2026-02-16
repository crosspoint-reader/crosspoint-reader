#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    goBack();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = CHECKING_FOR_UPDATE;
  xSemaphoreGive(renderingMutex);
  requestUpdateAndWait();

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    xSemaphoreGive(renderingMutex);
    requestUpdate();
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = NO_UPDATE;
    xSemaphoreGive(renderingMutex);
    requestUpdate();
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = WAITING_CONFIRMATION;
  xSemaphoreGive(renderingMutex);
  requestUpdate();
}

void OtaUpdateActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void OtaUpdateActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down
}

void OtaUpdateActivity::render(Activity::RenderLock&&) {
  if (subActivity) {
    // Subactivity handles its own rendering
    return;
  }

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Update", true, EpdFontFamily::BOLD);

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Checking for update...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, 200, "New update available!", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, 20, 250, "Current Version: " CROSSPOINT_VERSION);
    renderer.drawText(UI_10_FONT_ID, 20, 270, ("New Version: " + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels("Cancel", "Update", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 310, "Updating...", true, EpdFontFamily::BOLD);
    renderer.drawRect(20, 350, pageWidth - 40, 50);
    renderer.fillRect(24, 354, static_cast<int>(updaterProgress * static_cast<float>(pageWidth - 44)), 42);
    renderer.drawCenteredText(UI_10_FONT_ID, 420,
                              (std::to_string(static_cast<int>(updaterProgress * 100)) + "%").c_str());
    renderer.drawCenteredText(
        UI_10_FONT_ID, 440,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "No update available", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Update failed", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Update complete", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 350, "Press and hold power button to turn back on");
    renderer.displayBuffer();
    state = SHUTTING_DOWN;
    return;
  }
}

void OtaUpdateActivity::loop() {
  // TODO @ngxson : refactor this logic later
  if (updater.getRender()) {
    requestUpdate();
  }

  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdate();
      requestUpdateAndWait();
      const auto res = updater.installUpdate();

      if (res != OtaUpdater::OK) {
        LOG_DBG("OTA", "Update failed: %d", res);
        {
          RenderLock lock(*this);
          state = FAILED;
        }
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
