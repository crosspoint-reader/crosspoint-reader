#include "KOReaderSyncActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr time_t NTP_RESYNC_MIN_INTERVAL_SEC = 15 * 60;

// Emits heap snapshots around sync stages so we can correlate TLS failures with
// fragmentation and not just total free heap.
void logSyncMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const bool integrityOk = heap_caps_check_integrity_all(true);
  LOG_DBG("KOSync", "Sync mem[%s]: free=%lu contig=%lu integrity=%s", stage, freeHeap, contigHeap,
          integrityOk ? "ok" : "fail");
}

// Frees renderer-owned caches right before network work.
// Why: TLS handshake needs a large contiguous block, and font cache memory can
// increase fragmentation even when total free heap looks acceptable.
void trimMemoryBeforeTls(const GfxRenderer& renderer) {
  if (auto* cacheManager = renderer.getFontCacheManager()) {
    cacheManager->clearCache();
    cacheManager->resetStats();
    LOG_DBG("KOSync", "Cleared font cache before TLS");
  }
}

bool shouldSyncNtpNow() {
  const time_t lastSync = HalClock::lastSyncTime();
  const time_t now = HalClock::now();
  if (lastSync <= 0 || now <= 0) {
    return true;
  }

  const time_t age = now - lastSync;
  if (age < 0) {
    return true;
  }
  return age >= NTP_RESYNC_MIN_INTERVAL_SEC;
}
}  // namespace

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate();

  // Avoid repeated NTP churn during rapid sync retries; it can fragment heap
  // right before TLS. Re-sync only when clock is stale.
  if (shouldSyncNtpNow()) {
    HalClock::syncNtp();
  } else {
    LOG_DBG("KOSync", "Skipping NTP sync (recently synced)");
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate();

  logSyncMemSnapshot("before_performSync");
  trimMemoryBeforeTls(renderer);
  logSyncMemSnapshot("after_trim_before_performSync");

  performSync();

  logSyncMemSnapshot("after_performSync");
}

void KOReaderSyncActivity::performSync() {
  // Calculate document hash based on user's preferred method
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  // Local mapping is only needed for compare/upload paths.
  // Pull-only mode can skip this expensive step and go straight to remote fetch.
  if (syncIntent != SyncIntent::PULL_REMOTE) {
    // Precompute local mapping before first network request so the expensive
    // inflate/index work happens before TLS. This avoids a second local mapping
    // pass later and keeps the upload path lightweight.
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_MAPPING_LOCAL);
    }
    requestUpdateAndWait();
    computeLocalProgressAndChapter();
  }

  // Drop EPUB state before HTTPS to maximize contiguous heap for TLS.
  releaseEpubForMapping();

  // Push intent skips comparison UI but still warms an HTTP/TLS session first
  // so PUT can reuse the connection instead of forcing a fresh handshake.
  if (syncIntent == SyncIntent::PUSH_LOCAL) {
    // Direct push previously started with no reusable HTTP/TLS session, forcing
    // a fresh handshake in updateProgress. Compare flow often succeeds because
    // upload reuses the GET session. Warm the session here so push can take the
    // same reuse path without showing comparison UI.
    KOReaderSyncClient::beginPersistentSession();
    KOReaderProgress warmupProgress;
    const auto warmupResult = KOReaderSyncClient::getProgress(documentHash, warmupProgress);
    if (warmupResult != KOReaderSyncClient::OK && warmupResult != KOReaderSyncClient::NOT_FOUND) {
      KOReaderSyncClient::endPersistentSession();
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = KOReaderSyncClient::errorString(warmupResult);
        const char* detail = KOReaderSyncClient::lastFailureDetail();
        if (detail && detail[0]) {
          statusMessage += " — ";
          statusMessage += detail;
        }
      }
      requestUpdate(true);
      return;
    }
    performUpload();
    return;
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdate();

  // Keep the GET connection alive so Upload can reuse the same session and
  // avoid a second TLS handshake under fragmented heap.
  KOReaderSyncClient::beginPersistentSession();

  // Fetch remote progress
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (syncIntent == SyncIntent::PULL_REMOTE) {
      // Pull intent must not silently fall back to upload when server has no
      // remote progress. Failing explicitly keeps action semantics predictable.
      KOReaderSyncClient::endPersistentSession();
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_NO_REMOTE_MSG);
      }
      requestUpdate(true);
      return;
    }

    // Keep session open so an immediate upload can reuse the same connection.
    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    KOReaderSyncClient::endPersistentSession();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      // Combine the short category label with the rich diagnostic so users (and bug
      // reports) can tell network/TLS/server/heap failures apart at a glance.
      statusMessage = KOReaderSyncClient::errorString(result);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate(true);
    return;
  }

  // Defer remote EPUB mapping until user chooses Apply. Upload only needs the
  // precomputed local XPath, so this avoids post-fetch inflate churn and keeps
  // the GET session reusable for PUT.
  hasRemoteProgress = true;
  remotePositionMapped = false;
  remotePosition.spineIndex = currentSpineIndex;
  remotePosition.pageNumber = 0;
  remotePosition.totalPages = 0;
  remotePosition.paragraphIndex = 0;
  remotePosition.hasParagraphIndex = false;
  int xpathSpineIndex = -1;
  if (ChapterXPathIndexer::tryExtractSpineIndexFromXPath(remoteProgress.progress, xpathSpineIndex) &&
      xpathSpineIndex >= 0) {
    remotePosition.spineIndex = xpathSpineIndex;
  }
  remoteChapterLabel = tr(STR_UNNAMED);

  if (syncIntent == SyncIntent::PULL_REMOTE) {
    // Pull intent applies immediately and exits. We bypass chooser UI to keep
    // reader menu actions deterministic ("pull" always means apply remote).
    if (!ensureRemotePositionMapped()) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }

    // Preserve the apply result and show explicit confirmation before returning
    // to the reader so users can tell pull succeeded.
    setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber, remotePosition.paragraphIndex,
                         remotePosition.hasParagraphIndex});
    {
      RenderLock lock(*this);
      state = APPLY_COMPLETE;
      uploadCompleteTime = millis();
    }
    requestUpdate(true);
    return;
  }

  // Compare intent keeps the legacy chooser flow (apply vs upload), which is
  // still useful for manual conflict decisions.
  // Local progress was precomputed before network; keep using the cached value.
  releaseEpubForMapping();

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // If sync reached this screen without cached local progress, compute it now.
  // This keeps upload robust when UI flow changes or retries happen.
  if (localProgress.xpath.empty()) {
    computeLocalProgressAndChapter();
    releaseEpubForMapping();
  }

  // Hard-stop if we still have no xpath: sending an empty progress payload would
  // be ambiguous server-side and hides the real local mapping failure.
  if (localProgress.xpath.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Result screen rendering repopulates glyph caches; trim again right before
  // the upload handshake to maximize contiguous heap for TLS.
  trimMemoryBeforeTls(renderer);
  logSyncMemSnapshot("after_trim_before_updateProgress");

  // Capture upload-phase memory separately from fetch phase to diagnose failures
  // that only appear on PUT due to allocator state changes.
  logSyncMemSnapshot("before_updateProgress");

  // Ensure a session exists for upload. In compare flow this comes from the
  // earlier GET; in direct-push flow it comes from the warmup GET above.
  // In both cases, reuse avoids a second full TLS handshake.
  KOReaderSyncClient::beginPersistentSession();

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  const auto result = KOReaderSyncClient::updateProgress(progress);
  KOReaderSyncClient::endPersistentSession();
  logSyncMemSnapshot("after_updateProgress");

  if (result != KOReaderSyncClient::OK) {
    HalClock::wifiOff(true);
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      // Combine the short category label with the rich diagnostic so users (and bug
      // reports) can tell network/TLS/server/heap failures apart at a glance.
      statusMessage = KOReaderSyncClient::errorString(result);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate();
    return;
  }

  HalClock::wifiOff(true);
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
    uploadCompleteTime = millis();
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  KOReaderSyncClient::endPersistentSession();
  HalClock::wifiOff(true);
}

void KOReaderSyncActivity::closeCancelled() {
  if (closeRequested) {
    return;
  }

  closeRequested = true;
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void KOReaderSyncActivity::render(RenderLock&&) {
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15 + contentRect.y, tr(STR_KOREADER_SYNC), true, EpdFontFamily::BOLD);

  if (state == NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_KOREADER_SETUP_HINT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    GUI.drawPopup(renderer, statusMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Get chapter names from TOC
    const std::string& remoteChapter = remoteChapterLabel;
    const std::string& localChapter = localChapterLabel;

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 210, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 235, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(contentRect.x, optionY - 2, contentRect.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(contentRect.x, optionY + optionHeight - 2, contentRect.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, optionY + optionHeight, tr(STR_UPLOAD_LOCAL),
                      selectedOption != 1);

    // Bottom button hints
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

  if (state == APPLY_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_PULL_SUCCESS), true, EpdFontFamily::BOLD);

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

bool KOReaderSyncActivity::ensureEpubLoadedForMapping() {
  if (epub) {
    return true;
  }

  // Reload on demand to keep steady-state sync memory low. Mapping and chapter
  // lookup need EPUB metadata; TLS steps do not.
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!epub->load(true, true)) {
    LOG_ERR("KOSync", "Failed to reload EPUB for mapping: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  epub->setupCacheDir();
  return true;
}

bool KOReaderSyncActivity::ensureRemotePositionMapped() {
  if (remotePositionMapped) {
    return true;
  }

  // Apply needs remote->local mapping, which triggers EPUB inflate work.
  // Release HTTP/TLS session first so mapping has maximum heap headroom.
  KOReaderSyncClient::endPersistentSession();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_MAPPING_REMOTE);
  }
  requestUpdateAndWait();

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  if (!ensureEpubLoadedForMapping()) {
    return false;
  }
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);
  computeRemoteChapter();
  releaseEpubForMapping();
  remotePositionMapped = true;
  return true;
}

void KOReaderSyncActivity::releaseEpubForMapping() { epub.reset(); }

void KOReaderSyncActivity::computeLocalProgressAndChapter() {
  if (!ensureEpubLoadedForMapping()) {
    return;
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine, localParagraphIndex,
                                 hasLocalParagraphIndex};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterLabel = (localTocIndex >= 0)
                          ? epub->getTocItem(localTocIndex).title
                          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));
}

void KOReaderSyncActivity::computeRemoteChapter() {
  if (!epub) {
    return;
  }
  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  remoteChapterLabel = (remoteTocIndex >= 0)
                           ? epub->getTocItem(remoteTocIndex).title
                           : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == APPLY_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // APPLY_COMPLETE already has a valid SyncResult, so exit normally.
      // Other terminal states are treated as cancelled when backing out.
      if (state == APPLY_COMPLETE) {
        finish();
      } else {
        closeCancelled();
      }
      return;
    }

    if ((state == UPLOAD_COMPLETE || state == APPLY_COMPLETE) && millis() - uploadCompleteTime >= 3000) {
      // Keep pull/apply result on auto-close; upload-complete remains cancel-style.
      if (state == APPLY_COMPLETE) {
        finish();
      } else {
        closeCancelled();
      }
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        if (!ensureRemotePositionMapped()) {
          {
            RenderLock lock(*this);
            state = SYNC_FAILED;
            statusMessage = tr(STR_SYNC_FAILED_MSG);
          }
          requestUpdate(true);
          return;
        }
        // Wifi will be turned off in onExit()
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber, remotePosition.paragraphIndex,
                             remotePosition.hasParagraphIndex});
        finish();
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
    }
    return;
  }
}
