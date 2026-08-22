#include "ReadwiseSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "network/ReadwiseClient.h"
#include "EpubComposer.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadwiseSyncActivity::onEnter() {
  Activity::onEnter();

  if (SETTINGS.readwiseApiKey[0] == '\0') {
    state_ = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  shouldTearDownWifiOnExit_ = true;
  launchWifiSelection();
}

void ReadwiseSyncActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ReadwiseSyncActivity::launchWifiSelection() {
  LOG_INF("RWISE", "Sync requested without WiFi, launching WiFi selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadwiseSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("RWISE", "WiFi selection cancelled before sync");
    finish();
    return;
  }
  state_ = SYNCING;
  requestUpdate();
}

bool ReadwiseSyncActivity::downloadOne(const char* id, const char* title) {
  char path[160];
  ReadwiseClient::buildEpubPath(title, id, path, sizeof(path));

  if (Storage.exists(path)) {
    skippedCount_++;
    return true;
  }

  char progressBuf[96];
  snprintf(progressBuf, sizeof(progressBuf), "[%d] %s", downloadedCount_ + skippedCount_ + 1,
           title[0] != '\0' ? title : "...");
  statusLine_ = progressBuf;
  requestUpdate(true);

  EpubComposer composer(path);
  if (!composer.begin() || !composer.beginContent()) {
    errorDetail_ = "cannot write to /Readwise";
    return false;
  }

  ReadwiseClient::DocumentContent meta = {};
  const auto result = ReadwiseClient::downloadDocument(SETTINGS.readwiseApiKey, id, composer, meta, &cancelRequested_);
  if (result != ReadwiseClient::OK) {
    composer.abort();
    errorDetail_ = result == ReadwiseClient::CANCELLED ? "" : ReadwiseClient::errorString(result);
    return false;
  }
  if (!composer.endContent(meta.title[0] != '\0' ? meta.title : "Untitled", meta.author, id)) {
    errorDetail_ = "SD write failed";
    return false;
  }
  downloadedCount_++;
  return true;
}

void ReadwiseSyncActivity::performSync() {
  const char* apiKey = SETTINGS.readwiseApiKey;
  const char* tag = SETTINGS.readwiseTag;

  char cursor[192] = "";
  bool morePages = true;
  while (morePages && !cancelRequested_) {
    // Pump input between pages so Back can cancel even while listing runs.
    mappedInput.update();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested_ = true;

    statusLine_ = tr(STR_READWISE_CHECKING);
    requestUpdate(true);

    std::vector<ReadwiseClient::DocMeta> items;
    char nextCursor[192] = "";
    const auto result = ReadwiseClient::listPage(apiKey, tag, cursor, items, nextCursor, sizeof(nextCursor),
                                                 &cancelRequested_);
    if (result != ReadwiseClient::OK) {
      if (result == ReadwiseClient::CANCELLED) {
        state_ = CANCELLED;
      } else {
        errorDetail_ = ReadwiseClient::errorString(result);
        state_ = FAILED;
      }
      return;
    }

    for (const auto& item : items) {
      if (cancelRequested_) break;
      if (!downloadOne(item.id, item.title)) {
        if (cancelRequested_) {
          state_ = CANCELLED;
        } else {
          state_ = FAILED;
        }
        return;
      }
    }

    morePages = nextCursor[0] != '\0';
    snprintf(cursor, sizeof(cursor), "%s", nextCursor);
  }

  state_ = cancelRequested_ ? CANCELLED : SUCCESS;
}

void ReadwiseSyncActivity::loop() {
  if (state_ == SYNCING && !started_) {
    // First tick: render the syncing screen, then run the blocking sync.
    requestUpdateAndWait();
    started_ = true;
    performSync();
    requestUpdate(true);
    return;
  }

  if (state_ == SYNCING) {
    return;  // blocking sync runs above; terminal states fall through to input
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ReadwiseSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READWISE));

  const int midY = pageHeight / 2;

  switch (state_) {
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, statusLine_.c_str());
      break;
    case NO_CREDENTIALS:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_READWISE_NO_KEY), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_READWISE_NO_KEY_HINT));
      break;
    case SUCCESS: {
      char doneBuf[64];
      snprintf(doneBuf, sizeof(doneBuf), tr(STR_READWISE_DONE_FORMAT), downloadedCount_, skippedCount_);
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, doneBuf, true, EpdFontFamily::BOLD);
      break;
    }
    case CANCELLED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_READWISE_CANCELLED));
      break;
    case FAILED: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
      if (!errorDetail_.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, errorDetail_.c_str());
      }
      break;
    }
  }

  if (state_ != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
