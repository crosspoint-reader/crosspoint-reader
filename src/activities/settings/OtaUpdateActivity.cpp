#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaBootCheck.h"

void OtaUpdateActivity::consumeBootResult(const OtaBootCheck::Result& result) {
  {
    RenderLock lock(*this);
    if (result.error == OtaUpdater::OK) {
      updater.adoptManifest(result.version, result.url, result.size);
      state = updater.isUpdateNewer() ? WAITING_CONFIRMATION : NO_UPDATE;
    } else if (result.error == OtaUpdater::NO_UPDATE) {
      state = NO_UPDATE;
    } else {
      state = FAILED;
    }
  }
  requestUpdate(true);
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  // The selection saved the credential; the boot stage reconnects with it.
  OtaBootCheck::requestCheck();
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Landing here after a boot-time stage: show its outcome.
  if (const auto* bootResult = OtaBootCheck::takeResult()) {
    LOG_DBG("OTA", "Consuming boot stage result: error=%d", static_cast<int>(bootResult->error));
    consumeBootResult(*bootResult);
    return;
  }

  // Fresh entry from Settings: hand the check to the next boot, where the TLS
  // handshake has enough heap. Needs a saved credential to reconnect with.
  if (OtaBootCheck::canAutoConnect()) {
    {
      RenderLock lock(*this);
      state = CHECKING_FOR_UPDATE;
    }
    requestUpdateAndWait();
    OtaBootCheck::requestCheck();  // does not return
    return;
  }

  // No saved network yet: run the selection UI once to capture a credential.
  LOG_DBG("OTA", "No saved WiFi network, launching WifiSelectionActivity");
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Only the credential-capture path turns WiFi on in this activity; the boot
  // stages tear their connection down themselves. Silent-restart to free the
  // LWIP/TLS fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "Update confirmed, requesting boot-time install");
      OtaBootCheck::requestInstall(updater.getLatestVersion().c_str(), updater.getOtaUrl().c_str(),
                                   updater.getOtaSize());  // does not return
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED || state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
