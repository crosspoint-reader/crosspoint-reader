#include "MtgCardViewerActivity.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

MtgCardViewerActivity::MtgCardViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("MtgCardViewer", renderer, mappedInput) {}

unsigned long MtgCardViewerActivity::getIntervalMs() const {
  switch (SETTINGS.mtgSyncInterval) {
    case CrossPointSettings::MTG_SYNC_1_MIN: return 60000;
    case CrossPointSettings::MTG_SYNC_5_MIN: return 300000;
    case CrossPointSettings::MTG_SYNC_10_MIN: return 600000;
    case CrossPointSettings::MTG_SYNC_30_MIN: return 1800000;
    case CrossPointSettings::MTG_SYNC_1_HR: return 3600000;
    case CrossPointSettings::MTG_SYNC_1_DAY: return 86400000;
    default: return 0;
  }
}

void MtgCardViewerActivity::onEnter() {
  Activity::onEnter();
  state = ViewerState::VIEWING;
  lastSyncTime = millis();
  requestUpdate();
}

void MtgCardViewerActivity::onExit() {
  Activity::onExit();
  if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MtgCardViewerActivity::loop() {
  if (state == ViewerState::WIFI_SELECTION) {
    return;
  }

  if (state == ViewerState::SYNCING) {
    performSync();
    return;
  }

  if (state == ViewerState::CHECK_WIFI) {
    checkAndConnectWifi();
    return;
  }

  if (state == ViewerState::VIEWING) {
    // Check interval
    unsigned long interval = getIntervalMs();
    if (!isPaused && interval > 0 && (millis() - lastSyncTime >= interval)) {
      state = ViewerState::CHECK_WIFI;
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Toggle Pause
      isPaused = !isPaused;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      // Immediate manual pull
      state = ViewerState::CHECK_WIFI;
      requestUpdate();
    }
  }
}

void MtgCardViewerActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == ViewerState::VIEWING) {
    renderCard();
  } else {
    renderer.clearScreen();
    std::string title = tr(STR_MTG_SYNC);
    GUI.drawHeader(renderer, Rect{0, 20, pageWidth, 50}, title.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void MtgCardViewerActivity::renderCard() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  FsFile file;
  renderer.clearScreen();

  if (Storage.openFileForRead("BMP", "/.sleep/mtg_card.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x = 0, y = 0;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        }
      } else {
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid MTG Card BMP");
    }
    file.close();
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No MTG Card Synced");
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), isPaused ? "Unpause" : "Pause", "", "Pull Card");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (isPaused) {
    renderer.drawText(UI_10_FONT_ID, 10, pageHeight - 60, tr(STR_PAUSED));
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void MtgCardViewerActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = ViewerState::SYNCING;
    statusMessage = tr(STR_SYNCING);
    requestUpdate();
  } else {
    launchWifiSelection();
  }
}

void MtgCardViewerActivity::launchWifiSelection() {
  state = ViewerState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MtgCardViewerActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = ViewerState::SYNCING;
    statusMessage = tr(STR_SYNCING);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = ViewerState::VIEWING;
  }
  requestUpdate();
}

void MtgCardViewerActivity::performSync() {
  // Ensure we render the "Syncing..." message before blocking
  renderIfNeeded();

  Storage.mkdir("/.sleep");
  
  auto err = HttpDownloader::downloadToFile(
      "https://mtg-slab.sradentest.workers.dev/",
      "/.sleep/mtg_card.bmp");

  // We immediately return to viewing the card
  state = ViewerState::VIEWING;
  lastSyncTime = millis();
  
  // Optionally disconnect wifi here if we want to save battery between intervals
  if (getIntervalMs() == 0 || getIntervalMs() >= 300000) { // If interval >= 5 mins, turn off wifi
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }

  requestUpdate();
}
