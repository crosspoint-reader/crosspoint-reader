#include "BookFusionSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// Convert a BookFusion position (0-100 percentage) to CrossPoint using the KOReader
// percentage mapper. This avoids duplicating the spine-lookup logic.
CrossPointPosition bfToCrossPoint(const std::shared_ptr<Epub>& epub, const BookFusionPosition& bf,
                                  int currentSpineIndex, int totalPagesInCurrentSpine) {
  KOReaderPosition koPos;
  koPos.percentage = bf.percentage / 100.0f;
  koPos.xpath = "";
  return ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInCurrentSpine);
}

// Convert CrossPoint position to BookFusion format.
BookFusionPosition crossPointToBf(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  const KOReaderPosition koPos = ProgressMapper::toKOReader(epub, pos);
  const int spineCount = epub->getSpineItemsCount();

  BookFusionPosition bf;
  bf.percentage = koPos.percentage * 100.0f;
  bf.chapterIndex = pos.spineIndex;
  const float intraSpine = (pos.totalPages > 0) ? static_cast<float>(pos.pageNumber) / pos.totalPages : 0.0f;
  bf.pagePositionInBook = (spineCount > 0) ? (pos.spineIndex + intraSpine) / static_cast<float>(spineCount) : 0.0f;
  return bf;
}
}  // namespace

void BookFusionSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("BFSync", "WiFi connection failed");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  LOG_DBG("BFSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  performSync();
}

void BookFusionSyncActivity::performSync() {
  const auto result = BookFusionSyncClient::getProgress(bookId, remotePosition);

  if (result == BookFusionSyncClient::NOT_FOUND) {
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookFusionSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Convert remote BF position → CrossPoint
  hasRemoteProgress = true;
  remoteCrossPoint = bfToCrossPoint(epub, remotePosition, currentSpineIndex, totalPagesInSpine);

  // Compute local position in BF format for display
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  localPosition = crossPointToBf(epub, localPos);

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    // Default to whichever side is further ahead
    selectedOption = (localPosition.percentage > remotePosition.percentage) ? 1 : 0;
  }
  requestUpdate(true);
}

void BookFusionSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  const BookFusionPosition bf = crossPointToBf(epub, localPos);

  const auto result = BookFusionSyncClient::setProgress(bookId, bf);

  if (result != BookFusionSyncClient::OK) {
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookFusionSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  wifiOff();
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();

  if (!BF_TOKEN_STORE.hasToken()) {
    state = NO_TOKEN;
    requestUpdate();
    return;
  }

  bookId = BookFusionBookIdStore::loadBookId(epubPath.c_str());
  if (bookId == 0) {
    state = NOT_A_BF_BOOK;
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

void BookFusionSyncActivity::onExit() {
  Activity::onExit();
  wifiOff();
}

void BookFusionSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BF_SYNC), true, EpdFontFamily::BOLD);

  if (state == NO_TOKEN) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) - 20, tr(STR_BF_NO_TOKEN_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) + 10, tr(STR_BF_SETUP_HINT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NOT_A_BF_BOOK) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) - 10, tr(STR_BF_NOT_A_BF_BOOK), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                              statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remoteCrossPoint.spineIndex);
    const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remoteCrossPoint.spineIndex + 1));
    const std::string localChapter =
        (localTocIndex >= 0) ? epub->getTocItem(localTocIndex).title
                             : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    renderer.drawText(UI_10_FONT_ID, 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remoteCrossPoint.pageNumber + 1,
             remotePosition.percentage);
    renderer.drawText(UI_10_FONT_ID, 20, 210, remotePageStr);

    renderer.drawText(UI_10_FONT_ID, 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localPosition.percentage);
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
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) - 20, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) + 10, tr(STR_UPLOAD_PROMPT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                              tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) - 20, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight / 2) + 10, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void BookFusionSyncActivity::loop() {
  if (state == NO_TOKEN || state == NOT_A_BF_BOOK || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
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
        setResult(SyncResult{remoteCrossPoint.spineIndex, remoteCrossPoint.pageNumber});
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
