#include "ReadestSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReadestCredentialStore.h"
#include "ReadestSyncClient.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WifiUtils.h"

void ReadestSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  WifiUtils::syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void ReadestSyncActivity::performSync() {
  documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("RDSync", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  const auto result = ReadestSyncClient::getProgress(documentHash, remoteProgress);

  if (result == ReadestSyncClient::Error::NOT_FOUND) {
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
    }
    requestUpdate(true);
    return;
  }

  if (result != ReadestSyncClient::Error::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = ReadestSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = (localProgress.percentage > remoteProgress.percentage) ? 1 : 0;
  }
  requestUpdate(true);
}

void ReadestSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  KOReaderPosition koPos = ProgressMapper::toKOReader(epub, localPos);

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = koPos.xpath;
  progress.percentage = koPos.percentage;

  const auto result = ReadestSyncClient::updateProgress(progress);

  if (result != ReadestSyncClient::Error::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = ReadestSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void ReadestSyncActivity::onEnter() {
  Activity::onEnter();

  if (!READEST_STORE.hasCredentials() || READEST_STORE.getServerUrl().empty()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadestSyncActivity::onExit() {
  Activity::onExit();
  WifiUtils::wifiOff();
}

void ReadestSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_READEST_SYNC), true, EpdFontFamily::BOLD);

  if (state == NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_READEST_SETUP_HINT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
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
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        (localTocIndex >= 0) ? epub->getTocItem(localTocIndex).title
                             : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    renderer.drawText(UI_10_FONT_ID, 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 210, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, 20, 235, deviceStr);
    }

    renderer.drawText(UI_10_FONT_ID, 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    if (selectedOption == 0) {
      renderer.fillRect(0, optionY - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    if (selectedOption == 1) {
      renderer.fillRect(0, optionY + optionHeight - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ReadestSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber});
        finish();
      } else {
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }
}
