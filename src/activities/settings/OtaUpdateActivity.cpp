#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      failureReason = res;
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  HalClock::wifiOff();
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = contentRect.y + (contentRect.height - height) / 2;

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

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding,
                      top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer,
                        Rect{contentRect.x + metrics.contentSidePadding, y,
                             contentRect.width - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
                              (std::to_string(static_cast<int>(updaterProgress * 100)) + "%").c_str());
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    const char* reason = "";
    switch (failureReason) {
      case OtaUpdater::HTTP_ERROR:
        reason = "Network error (HTTP request failed)";
        break;
      case OtaUpdater::JSON_PARSE_ERROR:
        reason = "Could not parse release info";
        break;
      case OtaUpdater::UPDATE_OLDER_ERROR:
        reason = "Available version is not newer";
        break;
      case OtaUpdater::OOM_ERROR:
        reason = "Out of memory";
        break;
      case OtaUpdater::INTERNAL_UPDATE_ERROR:
        reason = "Internal update error";
        break;
      case OtaUpdater::NO_UPDATE:
        reason = "No firmware asset found";
        break;
      case OtaUpdater::VALIDATE_FAILED:
        reason = "Bootloader incompatible - reflash via USB with PlatformIO";
        break;
      default:
        break;
    }
    if (reason[0] != '\0') {
      renderer.drawCenteredText(SMALL_FONT_ID, top + height + metrics.verticalSpacing, reason);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  // TODO @ngxson : refactor this logic later
  if (updater.getRender()) {
    requestUpdate();
    updater.clearRender();
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      const auto beginResult = updater.beginInstallUpdate();
      if (beginResult != OtaUpdater::UPDATE_IN_PROGRESS) {
        LOG_DBG("OTA", "Update begin failed: %d", beginResult);
        {
          RenderLock lock(*this);
          failureReason = beginResult;
          state = FAILED;
        }
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == UPDATE_IN_PROGRESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      updater.cancelUpdate();
      finish();
      return;
    }

    const auto res = updater.performInstallUpdateStep();
    if (res == OtaUpdater::UPDATE_IN_PROGRESS) {
      if (updater.getRender()) {
        requestUpdate();
        updater.clearRender();
      }
      return;
    }

    if (res == OtaUpdater::OK) {
      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdate();
      return;
    }

    if (res == OtaUpdater::UPDATE_CANCELLED) {
      finish();
      return;
    }

    LOG_DBG("OTA", "Update failed: %d", res);
    {
      RenderLock lock(*this);
      failureReason = res;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
