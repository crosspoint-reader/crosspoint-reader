#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void syncTimeWithNTP() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}
}  // namespace

void KOReaderSyncActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KOReaderSyncActivity*>(param);
  self->displayTaskLoop();
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    onCancel();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = SYNCING;
  statusMessage = "Syncing time...";
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  statusMessage = "Calculating document hash...";
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

  performSync();
}

void KOReaderSyncActivity::performSync() {
  // Calculate document hash based on user's preferred method
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = SYNC_FAILED;
    statusMessage = "Failed to calculate document hash";
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  statusMessage = "Fetching remote progress...";
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
  vTaskDelay(10 / portTICK_PERIOD_MS);

  // Fetch remote progress
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // No remote progress - offer to upload
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = NO_REMOTE_PROGRESS;
    hasRemoteProgress = false;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = SYNC_FAILED;
    statusMessage = KOReaderSyncClient::errorString(result);
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  // Convert remote progress to CrossPoint position
  hasRemoteProgress = true;
  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, totalPagesInSpine);

  // Calculate local progress in KOReader format (for display)
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = SHOWING_RESULT;
  selectedOption = 0;  // Default to "Apply"
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
}

void KOReaderSyncActivity::performUpload() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = UPLOADING;
  statusMessage = "Uploading progress...";
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
  vTaskDelay(10 / portTICK_PERIOD_MS);

  // Convert current position to KOReader format
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  KOReaderPosition koPos = ProgressMapper::toKOReader(epub, localPos);

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = koPos.xpath;
  progress.percentage = koPos.percentage;

  const auto result = KOReaderSyncClient::updateProgress(progress);

  if (result != KOReaderSyncClient::OK) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = SYNC_FAILED;
    statusMessage = KOReaderSyncClient::errorString(result);
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = UPLOAD_COMPLETE;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
}

void KOReaderSyncActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  xTaskCreate(&KOReaderSyncActivity::taskTrampoline, "KOSyncTask",
              4096,               // Stack size (larger for network operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    updateRequired = true;
    return;
  }

  // Turn on WiFi
  LOG_DBG("KOSync", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    state = SYNCING;
    statusMessage = "Syncing time...";
    updateRequired = true;

    // Perform sync directly (will be handled in loop)
    xTaskCreate(
        [](void* param) {
          auto* self = static_cast<KOReaderSyncActivity*>(param);
          // Sync time first
          syncTimeWithNTP();
          xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
          self->statusMessage = "Calculating document hash...";
          xSemaphoreGive(self->renderingMutex);
          self->updateRequired = true;
          self->performSync();
          vTaskDelete(nullptr);
        },
        "SyncTask", 4096, this, 1, nullptr);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void KOReaderSyncActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void KOReaderSyncActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KOReaderSyncActivity::render() {
  if (subActivity) {
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "KOReader Sync", true, EpdFontFamily::BOLD);

  if (state == NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "No credentials configured", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, "Set up KOReader account in Settings");

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    renderer.drawCenteredText(UI_10_FONT_ID, 120, "Progress found!", true, EpdFontFamily::BOLD);

    // Get chapter names from TOC
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    const std::string remoteChapter = (remoteTocIndex >= 0)
                                          ? epub->getTocItem(remoteTocIndex).title
                                          : ("Section " + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter = (localTocIndex >= 0) ? epub->getTocItem(localTocIndex).title
                                                          : ("Section " + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, 20, 160, "Remote:", true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), "  Page %d, %.2f%% overall", remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 210, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), "  From: %s", remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, 20, 235, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, 20, 270, "Local:", true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), "  Page %d/%d, %.2f%% overall", currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(0, optionY - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY, "Apply remote progress", selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(0, optionY + optionHeight - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, "Upload local progress", selectedOption != 1);

    // Bottom button hints: show Back and Select
    const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "No remote progress found", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, "Upload current position?");

    const auto labels = mappedInput.mapLabels("Back", "Upload", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Progress uploaded!", true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "Sync failed", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onCancel();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      updateRequired = true;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        // Apply remote progress
        onSyncComplete(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onCancel();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onCancel();
    }
    return;
  }
}
