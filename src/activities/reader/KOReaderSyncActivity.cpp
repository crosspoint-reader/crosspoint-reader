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

// Frees renderer-owned caches inside the standalone sync activity right before
// network work. The reader activity is already gone by this point, but sync UI
// rendering (status popups, compare screen, result screen) can repopulate font
// caches and chip away at the largest free block needed for TLS.
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
    LOG_DBG("KOSync", "WiFi connection failed, resuming reader");
    resumeReader(KOReaderSyncOutcomeState::CANCELLED);
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
  if (syncIntent != KOReaderSyncIntentState::PULL_REMOTE) {
    // Precompute local mapping before first network request so the expensive
    // inflate/index work happens before TLS. This avoids a second local mapping
    // pass later and keeps the upload path lightweight.
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_MAPPING_LOCAL);
    }
    requestUpdateAndWait();
    if (!computeLocalProgressAndChapter()) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
  }

  // Drop EPUB state before HTTPS to maximize contiguous heap for TLS.
  releaseEpubForMapping();

  // Push intent skips comparison UI but still warms an HTTP/TLS session first
  // so PUT can reuse the connection instead of forcing a fresh handshake.
  if (syncIntent == KOReaderSyncIntentState::PUSH_LOCAL) {
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
    if (syncIntent == KOReaderSyncIntentState::PULL_REMOTE) {
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

  // Prepare remote mapping state for the next step.
  hasRemoteProgress = false;
  remotePositionMapped = false;
  remotePosition.spineIndex = -1;
  remotePosition.pageNumber = -1;
  remotePosition.totalPages = 0;
  remotePosition.paragraphIndex = 0;
  remotePosition.hasParagraphIndex = false;
  remoteChapterLabel.clear();

  if (syncIntent == KOReaderSyncIntentState::PULL_REMOTE) {
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
    auto& sync = APP_STATE.koReaderSyncSession;
    sync.outcome = KOReaderSyncOutcomeState::APPLIED_REMOTE;
    sync.resultSpineIndex = remotePosition.spineIndex;
    sync.resultPage = remotePosition.pageNumber;
    sync.resultParagraphIndex = remotePosition.paragraphIndex;
    sync.resultHasParagraphIndex = remotePosition.hasParagraphIndex;
    APP_STATE.saveToFile();
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
  // Pre-map remote progress now so compare UI always shows concrete chapter/
  // page data. The mapped result is cached and reused if Apply is chosen.
  if (!ensureRemotePositionMapped(false)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

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
    if (!computeLocalProgressAndChapter()) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
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

  // Sync UI rendering can repopulate glyph caches after the initial GET / compare
  // phase, so trim again right before the upload request.
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
  APP_STATE.koReaderSyncSession.outcome = KOReaderSyncOutcomeState::UPLOAD_COMPLETE;
  APP_STATE.saveToFile();
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
    uploadCompleteTime = millis();
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  logSyncMemSnapshot("onEnter_begin");
  LOG_DBG("KOSync", "Standalone sync start: path=%s spine=%d page=%d/%d intent=%d", epubPath.c_str(), currentSpineIndex,
          currentPage, totalPagesInSpine, static_cast<int>(syncIntent));

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

  logSyncMemSnapshot("onExit_before_cleanup");
  KOReaderSyncClient::endPersistentSession();
  HalClock::wifiOff(true);
  releaseEpubForMapping();
  logSyncMemSnapshot("onExit_after_cleanup");
}

void KOReaderSyncActivity::closeCancelled() {
  if (closeRequested) {
    return;
  }

  resumeReader(KOReaderSyncOutcomeState::CANCELLED);
}

void KOReaderSyncActivity::resumeReader(const KOReaderSyncOutcomeState outcome, const SyncResult* appliedResult) {
  if (closeRequested) {
    return;
  }

  closeRequested = true;
  auto& sync = APP_STATE.koReaderSyncSession;
  sync.outcome = outcome;
  if (appliedResult) {
    sync.resultSpineIndex = appliedResult->spineIndex;
    sync.resultPage = appliedResult->page;
    sync.resultParagraphIndex = appliedResult->paragraphIndex;
    sync.resultHasParagraphIndex = appliedResult->hasParagraphIndex;
  } else {
    sync.resultSpineIndex = 0;
    sync.resultPage = 0;
    sync.resultParagraphIndex = 0;
    sync.resultHasParagraphIndex = false;
  }
  APP_STATE.saveToFile();
  logSyncMemSnapshot("before_resume_reader");
  activityManager.goToReader(epubPath);
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

    // Word-wrap the detail message so long TLS/network diagnostics aren't clipped.
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), contentRect.width - 20, 4);
    int y = 320;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += lineHeight;
    }

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

bool KOReaderSyncActivity::ensureRemotePositionMapped(const bool closeSessionBeforeMapping) {
  if (remotePositionMapped) {
    return true;
  }

  // Mapping remote->local can trigger EPUB inflate work. For apply/pull paths,
  // release HTTP/TLS first to maximize heap headroom. Compare pre-map keeps
  // the warmed session alive so Upload can reuse it without a fresh handshake.
  if (closeSessionBeforeMapping) {
    KOReaderSyncClient::endPersistentSession();
  }

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
  hasRemoteProgress = true;
  remotePositionMapped = true;
  return true;
}

void KOReaderSyncActivity::releaseEpubForMapping() { epub.reset(); }

bool KOReaderSyncActivity::computeLocalProgressAndChapter() {
  if (!ensureEpubLoadedForMapping()) {
    localProgress = KOReaderPosition{};
    localChapterLabel.clear();
    return false;
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine, localParagraphIndex,
                                 hasLocalParagraphIndex};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterLabel = (localTocIndex >= 0)
                          ? epub->getTocItem(localTocIndex).title
                          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));
  return true;
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
      if (state == APPLY_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE);
      } else if (state == UPLOAD_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
      } else if (state == SYNC_FAILED || state == NO_CREDENTIALS) {
        resumeReader(KOReaderSyncOutcomeState::FAILED);
      } else {
        resumeReader(KOReaderSyncOutcomeState::CANCELLED);
      }
      return;
    }

    if ((state == UPLOAD_COMPLETE || state == APPLY_COMPLETE) && millis() - uploadCompleteTime >= 3000) {
      if (state == APPLY_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE);
      } else {
        resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
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
        const SyncResult result = {remotePosition.spineIndex, remotePosition.pageNumber, remotePosition.paragraphIndex,
                                   remotePosition.hasParagraphIndex};
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE, &result);
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
