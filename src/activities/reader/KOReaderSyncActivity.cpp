#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "AutoSleepSyncMarker.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method,
                                           const AutoSleepSyncDeadline* deadline) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path, deadline);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

const char* stageName(const KOReaderSyncStage stage) {
  switch (stage) {
    case KOReaderSyncStage::NTP:
      return "ntp";
    case KOReaderSyncStage::PRIMARY_HASH:
      return "primary-hash";
    case KOReaderSyncStage::PRIMARY_GET:
      return "primary-get";
    case KOReaderSyncStage::ALTERNATE_HASH:
      return "alternate-hash";
    case KOReaderSyncStage::ALTERNATE_GET:
      return "alternate-get";
    case KOReaderSyncStage::EPUB_RELOAD:
      return "epub-reload";
    case KOReaderSyncStage::REMOTE_MAPPING:
      return "remote-mapping";
    case KOReaderSyncStage::PUT:
      return "put";
  }
  return "unknown";
}

const char* terminalReasonName(const KOReaderSyncTerminalReason reason) {
  switch (reason) {
    case KOReaderSyncTerminalReason::WIFI_FAILED:
      return "wifi-failed";
    case KOReaderSyncTerminalReason::ACTIVITY_OOM:
      return "activity-oom";
    case KOReaderSyncTerminalReason::NO_CREDENTIALS:
      return "no-credentials";
    case KOReaderSyncTerminalReason::HASH_FAILED:
      return "hash-failed";
    case KOReaderSyncTerminalReason::GET_FAILED:
      return "get-failed";
    case KOReaderSyncTerminalReason::AUTH_FAILED:
      return "auth-failed";
    case KOReaderSyncTerminalReason::SERVER_FAILED:
      return "server-failed";
    case KOReaderSyncTerminalReason::JSON_FAILED:
      return "json-failed";
    case KOReaderSyncTerminalReason::LOW_MEMORY:
      return "low-memory";
    case KOReaderSyncTerminalReason::EPUB_LOAD_FAILED:
      return "epub-load-failed";
    case KOReaderSyncTerminalReason::SAVE_FAILED:
      return "save-failed";
    case KOReaderSyncTerminalReason::UPLOAD_FAILED:
      return "upload-failed";
    case KOReaderSyncTerminalReason::UPLOAD_COMPLETE:
      return "upload-complete";
    case KOReaderSyncTerminalReason::ALREADY_SYNCED:
      return "already-synced";
    case KOReaderSyncTerminalReason::REMOTE_APPLIED:
      return "remote-applied";
    case KOReaderSyncTerminalReason::DEADLINE_EXPIRED:
      return "deadline-expired";
  }
  return "unknown";
}

KOReaderSyncTerminalReason terminalReasonForError(const KOReaderSyncClient::Error error,
                                                  const KOReaderSyncTerminalReason fallback) {
  switch (error) {
    case KOReaderSyncClient::AUTH_FAILED:
      return KOReaderSyncTerminalReason::AUTH_FAILED;
    case KOReaderSyncClient::SERVER_ERROR:
      return KOReaderSyncTerminalReason::SERVER_FAILED;
    case KOReaderSyncClient::JSON_ERROR:
      return KOReaderSyncTerminalReason::JSON_FAILED;
    case KOReaderSyncClient::LOW_MEMORY:
      return KOReaderSyncTerminalReason::LOW_MEMORY;
    case KOReaderSyncClient::DEADLINE_EXPIRED:
      return KOReaderSyncTerminalReason::DEADLINE_EXPIRED;
    default:
      return fallback;
  }
}

bool syncTimeWithNTP(const KOReaderSyncRunMode runMode, const AutoSleepSyncDeadline deadline) {
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
  constexpr int MAX_RETRIES = 50;  // 5 seconds max for manual sync
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < MAX_RETRIES) {
    uint32_t delayMs = 100;
    if (runMode == KOReaderSyncRunMode::SLEEP) {
      delayMs = std::min<uint32_t>(deadline.remainingMs(millis()), 100u);
      if (delayMs == 0) return false;
    }
    vTaskDelay(std::max<uint32_t>(delayMs / portTICK_PERIOD_MS, 1));
    retry++;
  }

  if (retry < MAX_RETRIES) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
  return runMode == KOReaderSyncRunMode::MANUAL || !deadline.expired(millis());
}
}  // namespace

uint32_t KOReaderSyncActivity::elapsedMs() const {
  if (!isSleepMode()) return 0;
  const uint32_t startedAtMs = deadline.deadlineAtMs() - AutoSleepSyncDeadline::DEFAULT_BUDGET_MS;
  return millis() - startedAtMs;
}

bool KOReaderSyncActivity::routeTerminal(const KOReaderSyncTerminalReason reason,
                                         const KOReaderSyncTerminalAction manualAction) {
  // A successful sync (either mode) records the settled position so a later
  // eligible sleep at the same position can skip the preflight entirely.
  if (reason == KOReaderSyncTerminalReason::UPLOAD_COMPLETE || reason == KOReaderSyncTerminalReason::ALREADY_SYNCED ||
      reason == KOReaderSyncTerminalReason::REMOTE_APPLIED) {
    const bool remoteApplied = reason == KOReaderSyncTerminalReason::REMOTE_APPLIED;
    const int spine = remoteApplied ? remotePosition.spineIndex : currentSpineIndex;
    const int page = remoteApplied ? remotePosition.pageNumber : currentPage;
    const int total = remoteApplied ? remotePosition.totalPages : totalPagesInSpine;
    if (spine >= 0 && spine <= UINT16_MAX && page >= 0 && page <= UINT16_MAX && total >= 0 && total <= UINT16_MAX) {
      AutoSleepSyncMarkerData marker;
      marker.serverFingerprint = AutoSleepSyncMarker::serverFingerprint();
      marker.spineIndex = static_cast<uint16_t>(spine);
      marker.pageNumber = static_cast<uint16_t>(page);
      marker.totalPages = static_cast<uint16_t>(total);
      AutoSleepSyncMarker::save(AutoSleepSyncMarker::bookCachePathFor(epubPath), marker);
    }
  }

  const KOReaderSyncTerminalAction action = AutoSleepSyncPolicy::terminalAction(runMode, manualAction);
  if (action == KOReaderSyncTerminalAction::COMMIT_SLEEP) {
    epub.reset();
    LOG_DBG("KOSync", "Sleep sync terminal: reason=%s elapsed=%u heap=%u maxAlloc=%u", terminalReasonName(reason),
            elapsedMs(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (sleepCommitCallback) {
      sleepCommitCallback();
    } else {
      LOG_ERR("KOSync", "Sleep sync terminal callback missing");
    }
    return true;
  }
  if (action == KOReaderSyncTerminalAction::RETURN_TO_READER) {
    returnToReader();
    return true;
  }
  return false;
}

bool KOReaderSyncActivity::startStage(const KOReaderSyncStage stage) {
  if (!AutoSleepSyncPolicy::shouldStartStage(runMode, deadline, millis())) {
    routeTerminal(KOReaderSyncTerminalReason::DEADLINE_EXPIRED, KOReaderSyncTerminalAction::SHOW_RESULT);
    return false;
  }
  LOG_DBG("KOSync", "Stage=%s elapsed=%u", stageName(stage), elapsedMs());
  return true;
}

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    if (routeTerminal(KOReaderSyncTerminalReason::SAVE_FAILED, KOReaderSyncTerminalAction::SHOW_RESULT)) return;
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  routeTerminal(KOReaderSyncTerminalReason::REMOTE_APPLIED, KOReaderSyncTerminalAction::RETURN_TO_READER);
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  if (routeTerminal(KOReaderSyncTerminalReason::ALREADY_SYNCED, KOReaderSyncTerminalAction::SHOW_RESULT)) return;
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    routeTerminal(KOReaderSyncTerminalReason::WIFI_FAILED, KOReaderSyncTerminalAction::RETURN_TO_READER);
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  if (!isSleepMode()) {
    {
      RenderLock lock(*this);
      state = SYNCING;
      statusMessage = tr(STR_SYNCING_TIME);
    }
    requestUpdate(true);
  }

  // Sync time with NTP before making API requests.
  if (!startStage(KOReaderSyncStage::NTP)) return;
  if (!syncTimeWithNTP(runMode, deadline)) {
    routeTerminal(KOReaderSyncTerminalReason::DEADLINE_EXPIRED, KOReaderSyncTerminalAction::SHOW_RESULT);
    return;
  }

  if (!isSleepMode()) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_CALC_HASH);
    }
    requestUpdate(true);
  }

  performSync();
}

void KOReaderSyncActivity::performSync() {
  const AutoSleepSyncDeadline* sleepDeadline = isSleepMode() ? &deadline : nullptr;
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  if (!startStage(KOReaderSyncStage::PRIMARY_HASH)) return;
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod, sleepDeadline);
  if (documentHash.empty()) {
    // The hash aborts mid-operation on expiry; report the deadline, not a hash fault.
    const KOReaderSyncTerminalReason reason = (isSleepMode() && deadline.expired(millis()))
                                                  ? KOReaderSyncTerminalReason::DEADLINE_EXPIRED
                                                  : KOReaderSyncTerminalReason::HASH_FAILED;
    if (routeTerminal(reason, KOReaderSyncTerminalAction::SHOW_RESULT)) return;
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  if (!isSleepMode()) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_FETCH_PROGRESS);
    }
    requestUpdateAndWait();
  }

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the furthest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  if (!startStage(KOReaderSyncStage::PRIMARY_GET)) return;
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress, isSleepMode() ? &deadline : nullptr);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    if (!startStage(KOReaderSyncStage::ALTERNATE_HASH)) return;
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod, sleepDeadline);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      if (!startStage(KOReaderSyncStage::ALTERNATE_GET)) return;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress, isSleepMode() ? &deadline : nullptr);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        result = KOReaderSyncClient::OK;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

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
    if (routeTerminal(terminalReasonForError(result, KOReaderSyncTerminalReason::GET_FAILED),
                      KOReaderSyncTerminalAction::SHOW_RESULT)) {
      return;
    }
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  if (!startStage(KOReaderSyncStage::EPUB_RELOAD)) return;
  ensureEpubLoaded();
  if (!epub) {
    if (routeTerminal(KOReaderSyncTerminalReason::EPUB_LOAD_FAILED, KOReaderSyncTerminalAction::SHOW_RESULT)) return;
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  // Prefer the exact spine/page supplied by the CrossPoint sync server; fall
  // back to the approximate XPath mapping when it cannot be applied.
  std::optional<CrossPointPosition> richMapped;
  if (!startStage(KOReaderSyncStage::REMOTE_MAPPING)) return;
  if (remoteProgress.position.has_value()) {
    richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
  }
  if (richMapped.has_value()) {
    remotePosition = *richMapped;
  } else {
    SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition =
        ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine, 0, sleepDeadline);
  }
  // The XHTML scan inside toCrossPoint aborts on expiry with a fallback-quality
  // result; never act on it (save/upload) once the deadline has passed.
  if (isSleepMode() && deadline.expired(millis())) {
    routeTerminal(KOReaderSyncTerminalReason::DEADLINE_EXPIRED, KOReaderSyncTerminalAction::SHOW_RESULT);
    return;
  }

  if (smartSyncEnabled()) {
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: doc=%s local=%.6f remote=%.6f delta=%.6f remoteXpath=%s mapped=%d/%d",
            documentHash.c_str(), localProgress.percentage, remoteProgress.percentage, delta,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }

    if (delta > 0) {
      // Alternate hashes are only probes for newer remote state. Keep uploads
      // on the user's configured matching method so its primary record heals.
      documentHash = primaryHash;
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
    return;
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
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
  if (!isSleepMode()) {
    {
      RenderLock lock(*this);
      state = UPLOADING;
      statusMessage = tr(STR_UPLOAD_PROGRESS);
    }
    requestUpdateAndWait();
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Rich CrossPoint position for the default CrossPoint sync server (lossless
  // CrossPoint<->CrossPoint sync). The HTTP client also enforces this boundary
  // before serializing the extension.
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    if (!startStage(KOReaderSyncStage::EPUB_RELOAD)) return;
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  if (!startStage(KOReaderSyncStage::PUT)) return;
  const auto result = KOReaderSyncClient::updateProgress(progress, isSleepMode() ? &deadline : nullptr);

  // Manual mode drops the radio while the user reads the result. Sleep mode
  // leaves teardown to the one terminal commit path.
  if (!isSleepMode()) esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    if (routeTerminal(terminalReasonForError(result, KOReaderSyncTerminalReason::UPLOAD_FAILED),
                      KOReaderSyncTerminalAction::SHOW_RESULT)) {
      return;
    }
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  if (routeTerminal(KOReaderSyncTerminalReason::UPLOAD_COMPLETE, KOReaderSyncTerminalAction::SHOW_RESULT)) return;

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    if (routeTerminal(KOReaderSyncTerminalReason::NO_CREDENTIALS, KOReaderSyncTerminalAction::SHOW_RESULT)) return;
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth). Sleep preflight
  // may only use saved networks (R5), so an existing connection counts only when
  // its SSID is in the credential store; otherwise fall through to the headless
  // saved-WiFi flow, which disconnects and tries saved networks.
  if (WiFi.status() == WL_CONNECTED) {
    bool usableConnection = true;
    if (isSleepMode()) {
      WIFI_STORE.loadFromFile();
      usableConnection = WIFI_STORE.hasSavedCredential(std::string(WiFi.SSID().c_str()));
    }
    if (usableConnection) {
      LOG_DBG("KOSync", "Already connected to WiFi");
      onWifiSelectionComplete(true);
      return;
    }
    LOG_DBG("KOSync", "Connected network is not saved; running headless saved-WiFi flow");
  }

  // Sleep preflight uses only saved credentials and never presents interactive WiFi UI.
  if (runMode == KOReaderSyncRunMode::SLEEP) {
    LOG_DBG("KOSync", "Launching headless WifiSelectionActivity");
    // ActivityManager requires owned activity storage; nothrow allocation lets sleep commit on OOM.
    auto wifiActivity =
        makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true, WifiSelectionMode::HEADLESS, deadline);
    if (!wifiActivity) {
      LOG_ERR("KOSync", "OOM: headless WifiSelectionActivity");
      routeTerminal(KOReaderSyncTerminalReason::ACTIVITY_OOM, KOReaderSyncTerminalAction::SHOW_RESULT);
      return;
    }
    startActivityForResult(std::move(wifiActivity),
                           [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
    return;
  }

  LOG_DBG("KOSync", "Launching manual WifiSelectionActivity");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated && !isSleepMode()) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  // Automatic sleep sync is headless. In particular, Quick Resume must retain
  // the reader-context frame snapshotted before preflight until terminal restore.
  if (isSleepMode()) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  // Sleep is terminal: ignore navigation, retry, and repeated power input while
  // the coordinator waits to execute the single commit path.
  if (isSleepMode()) return;

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        performUpload();
      }
    };

    {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      constexpr int optionHeight = 30;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, top + 230 - 2, optionHeight, 2);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        chooseSelected();
        return;
      }
    }

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
      chooseSelected();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && ty > renderer.getScreenHeight() / 3 &&
        ty < renderer.getScreenHeight() * 2 / 3) {
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
      return;
    }

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
      returnToReader();
    }
    return;
  }
}
