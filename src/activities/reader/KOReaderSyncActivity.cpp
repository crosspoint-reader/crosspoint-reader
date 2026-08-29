#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "AutomaticProgressCheckPolicy.h"
#include "AutomaticProgressUploadPolicy.h"
#include "AutomaticWifiConnectionPolicy.h"
#include "DeepSleep.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "MappedProgressPositionPolicy.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"  // list icons for the compare rows
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// One action id for both interactive states: the compare rows (SHOWING_RESULT)
// and the upload button (NO_REMOTE_PROGRESS) never coexist, so state
// disambiguates them in the handler.
constexpr fui::ActionId ACTION_ROW = 1;

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

}  // namespace

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath, const CrossPointPosition& localPosition,
                                           SavedProgressPosition localKoPos, std::string localChapterName,
                                           std::optional<uint16_t> currentParagraphIndex, Mode mode,
                                           CompletionTarget completionTarget,
                                           std::optional<KOReaderProgress> prefetchedRemoteProgress,
                                           std::optional<CrossPointPosition> prefetchedRemotePosition)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      localChapterName(std::move(localChapterName)),
      currentSpineIndex(localPosition.spineIndex),
      currentPage(localPosition.pageNumber),
      totalPagesInSpine(localPosition.totalPages),
      currentParagraphIndex(currentParagraphIndex),
      remoteProgress{},
      remotePosition{},
      localProgress(std::move(localKoPos)),
      mode(mode),
      completionTarget(completionTarget) {
  if (prefetchedRemoteProgress.has_value() && prefetchedRemotePosition.has_value()) {
    remoteProgress = std::move(*prefetchedRemoteProgress);
    remotePosition = *prefetchedRemotePosition;
    prefetchedResult = true;
  }
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
  std::optional<uint32_t> offset;
  if (remotePosition.hasVisibleTextOffset && remotePosition.spineIndex == spineIndex) {
    offset = remotePosition.visibleTextOffset;
  }
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, offset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  completeFlow();
}

void KOReaderSyncActivity::completeFlow() {
  switch (completionTarget) {
    case CompletionTarget::HOME:
      activityManager.goHome();
      break;
    case CompletionTarget::FILE_BROWSER:
      activityManager.goToFileBrowser(epubPath);
      break;
    case CompletionTarget::SLEEP:
      completeDeferredDeepSleep(false);
      break;
    case CompletionTarget::SLEEP_TIMEOUT:
      completeDeferredDeepSleep(true);
      break;
    case CompletionTarget::READER:
    default:
      activityManager.goToReader(epubPath);
      break;
  }
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return mode == Mode::MANUAL && KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

uint32_t KOReaderSyncActivity::automaticOperationRemainingMs() const {
  return AutomaticWifiConnectionPolicy::remainingBackgroundTimeMs(millis(), automaticOperationStartedAt);
}

bool KOReaderSyncActivity::automaticOperationDeadlineExpired() const {
  return automaticPush() && automaticOperationRemainingMs() == 0;
}

KOReaderSyncClient::Error KOReaderSyncActivity::getProgress(const std::string& hash, KOReaderProgress& progress) {
  if (!automaticPush()) {
    return KOReaderSyncClient::getProgress(hash, progress);
  }

  const uint32_t remaining = automaticOperationRemainingMs();
  if (remaining == 0) {
    LOG_DBG("KOSync", "Automatic upload deadline reached before progress lookup");
    return KOReaderSyncClient::NETWORK_ERROR;
  }
  return KOReaderSyncClient::getProgress(hash, progress, remaining,
                                         [this]() { return automaticOperationDeadlineExpired(); });
}

KOReaderSyncClient::Error KOReaderSyncActivity::updateProgress(const KOReaderProgress& progress) {
  if (!automaticPush()) {
    return KOReaderSyncClient::updateProgress(progress);
  }

  const uint32_t remaining = automaticOperationRemainingMs();
  if (remaining == 0) {
    LOG_DBG("KOSync", "Automatic upload deadline reached before progress update");
    return KOReaderSyncClient::NETWORK_ERROR;
  }
  return KOReaderSyncClient::updateProgress(progress, remaining,
                                            [this]() { return automaticOperationDeadlineExpired(); });
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    completeFlow();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Keep the station fully awake for the short sync transaction. The web server
  // does the same because ESP32 modem sleep can introduce multi-second network
  // stalls that surface as HTTP timeouts. WiFi is torn down when this activity exits.
  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  // KOSync requests from CrossPoint do not include a client timestamp.
  performSync();
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    if (automaticMode()) {
      completeFlow();
      return;
    }
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

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the furthest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  auto result = getProgress(documentHash, remoteProgress);
  std::optional<KOReaderProgress> alternateRemoteProgress;
  std::string alternateDocumentHash;
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  if (smartSyncEnabled() || automaticPush()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (automaticPush() && altResult != KOReaderSyncClient::OK && altResult != KOReaderSyncClient::NOT_FOUND) {
        LOG_ERR("KOSync", "Aborting automatic upload because alternate document lookup failed: %d", altResult);
        completeFlow();
        return;
      }

      if (altResult == KOReaderSyncClient::OK) {
        if (result == KOReaderSyncClient::NOT_FOUND) {
          documentHash = altHash;
          remoteProgress = std::move(altProgress);
          result = KOReaderSyncClient::OK;
        } else if (automaticPush() && result == KOReaderSyncClient::OK) {
          alternateDocumentHash = altHash;
          alternateRemoteProgress.emplace(std::move(altProgress));
        } else if (smartSyncEnabled() && altProgress.percentage > remoteProgress.percentage) {
          documentHash = altHash;
          remoteProgress = std::move(altProgress);
        }
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (automaticPull()) {
      completeFlow();
      return;
    }
    if (automaticPush()) {
      if (AutomaticProgressUploadPolicy::decide(localProgress.percentage, false, 0.0f) !=
          AutomaticProgressUploadDecision::UPLOAD) {
        LOG_ERR("KOSync", "Skipping automatic upload with invalid local progress: %.6f", localProgress.percentage);
        completeFlow();
        return;
      }
      documentHash = primaryHash;
      performUpload();
      return;
    }

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
    if (automaticMode()) {
      completeFlow();
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

  if (automaticPull() && AutomaticProgressCheckPolicy::decide(localProgress.percentage, remoteProgress.percentage) ==
                             AutomaticProgressDecision::INVALID_REMOTE) {
    LOG_ERR("KOSync", "Ignoring invalid remote percentage: %.6f", remoteProgress.percentage);
    completeFlow();
    return;
  }
  if (automaticPush() &&
      AutomaticProgressUploadPolicy::decide(localProgress.percentage, true, remoteProgress.percentage) ==
          AutomaticProgressUploadDecision::INVALID_PROGRESS) {
    LOG_ERR("KOSync", "Skipping automatic upload with invalid progress: local=%.6f remote=%.6f",
            localProgress.percentage, remoteProgress.percentage);
    completeFlow();
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    if (automaticMode()) {
      completeFlow();
      return;
    }
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  // The standard KOReader progress XPath is the authoritative content anchor.
  // The CrossPoint server's existing rich page hints remain a legacy fallback.
  const auto mapRemotePosition = [this](const KOReaderProgress& progress) {
    const SavedProgressPosition koPos = {progress.progress, progress.percentage};
    CrossPointPosition mapped =
        ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
    if (!mapped.hasVisibleTextOffset && progress.position.has_value()) {
      // toCrossPoint above already tried koPos.xpath; if the rich position carries the same XPath,
      // tell fromRichPosition to skip re-resolving it and use its page hints directly.
      const bool sameXPath = progress.position->xpath == progress.progress;
      if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *progress.position, renderer, sameXPath)) {
        mapped = *richMapped;
      }
    }
    return mapped;
  };
  remotePosition = mapRemotePosition(remoteProgress);

  if (automaticPush() && alternateRemoteProgress.has_value()) {
    const CrossPointPosition alternatePosition = mapRemotePosition(*alternateRemoteProgress);
    const auto alternateOrder =
        MappedProgressPositionPolicy::compare(remotePosition.spineIndex, remotePosition.pageNumber,
                                              alternatePosition.spineIndex, alternatePosition.pageNumber);
    if (alternateOrder == MappedProgressPositionOrder::REMOTE_AHEAD) {
      LOG_DBG("KOSync", "Alternate mapped remote is ahead: primary=%d/%d alternate=%d/%d", remotePosition.spineIndex,
              remotePosition.pageNumber, alternatePosition.spineIndex, alternatePosition.pageNumber);
      documentHash = alternateDocumentHash;
      remoteProgress = std::move(*alternateRemoteProgress);
      remotePosition = alternatePosition;
    }
  }

  const auto mappedOrder = MappedProgressPositionPolicy::compare(currentSpineIndex, currentPage,
                                                                 remotePosition.spineIndex, remotePosition.pageNumber);
  LOG_DBG("KOSync", "Mapped decision: local=%d/%d remote=%d/%d order=%d", currentSpineIndex, currentPage,
          remotePosition.spineIndex, remotePosition.pageNumber, static_cast<int>(mappedOrder));

  if (automaticPull()) {
    if (mappedOrder != MappedProgressPositionOrder::REMOTE_AHEAD) {
      completeFlow();
      return;
    }

    // The prompt can remain visible while the user decides; drop the radio now
    // instead of keeping WiFi powered for an interaction that needs no network.
    WiFi.disconnect(true, false);
    {
      RenderLock lock(*this);
      state = SHOWING_RESULT;
      selectedOption = 0;  // Resume remotely by default.
    }
    requestUpdate(true);
    return;
  }

  if (automaticPush()) {
    epub.reset();
    if (mappedOrder != MappedProgressPositionOrder::LOCAL_AHEAD) {
      completeFlow();
      return;
    }

    // Alternate hashes are probes only. Upload to the configured primary ID.
    documentHash = primaryHash;
    performUpload();
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
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

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

  const auto result = updateProgress(progress);

  // Drop the radio while the user reads the result. Use the framework API so
  // its internal started/mode state stays consistent for a later reconnect.
  WiFi.disconnect(true, false);

  if (result != KOReaderSyncClient::OK) {
    if (automaticPush()) {
      completeFlow();
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

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (automaticPush()) {
    completeFlow();
    return;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  if (automaticPush()) {
    automaticOperationStartedAt = millis();
  }
  LOG_DBG("KOSync", "Sync activity local position: spine=%d page=%d total=%d", currentSpineIndex, currentPage,
          totalPagesInSpine);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  app.on(ACTION_ROW, &KOReaderSyncActivity::onResultRow, this);
  app.setScreen(&KOReaderSyncActivity::resultScreen, this);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    if (automaticMode()) {
      completeFlow();
      return;
    }
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // A background automatic check already fetched the remote progress; skip the
  // network and go straight to the result screen.
  if (prefetchedResult) {
    hasRemoteProgress = true;
    ensureEpubLoaded();  // needed for the chapter title in the result screen
    {
      RenderLock lock(*this);
      state = SHOWING_RESULT;
      selectedOption = 0;  // Resume remotely by default.
    }
    requestUpdate(true);
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  const WifiAutoConnectMode wifiMode = automaticPush()   ? WifiAutoConnectMode::HEADLESS_BACKGROUND
                                       : automaticPull() ? WifiAutoConnectMode::HEADLESS_QUICK
                                                         : WifiAutoConnectMode::INTERACTIVE;
  const std::optional<uint32_t> backgroundStartedAt =
      automaticPush() ? std::optional<uint32_t>(automaticOperationStartedAt) : std::nullopt;
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, wifiMode, backgroundStartedAt),
      [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    if (automaticMode()) {
      // A raw esp_wifi_stop() leaves Arduino WiFi's internal state marked as
      // started, causing a later automatic sleep upload to skip radio startup.
      WiFi.disconnect(true, false);
      delay(30);
      return;
    }
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::chooseResultOption() {
  if (selectedOption == 0) {
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
  } else if (automaticPull()) {
    completeFlow();
  } else {
    performUpload();
  }
}

void KOReaderSyncActivity::startUpload() {
  if (documentHash.empty()) {
    documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                       ? KOReaderDocumentId::calculateFromFilename(epubPath)
                       : KOReaderDocumentId::calculate(epubPath);
  }
  performUpload();
}

void KOReaderSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KOReaderSyncActivity*>(user);
  // Activation leaves this screen (applies/uploads); drop the flash so it does
  // not ghost onto the next paint.
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    if (event.value < 0 || event.value > 1) return;
    self->selectedOption = event.value;
    self->chooseResultOption();
  } else if (self->state == NO_REMOTE_PROGRESS) {
    self->startUpload();
  }
}

void KOReaderSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<KOReaderSyncActivity*>(user)->buildResultScreen(screen);
}

void KOReaderSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Side padding is 0 here (like the other FreeInkApp screens): the action list
  // supplies its own theme side padding, and the raw comparison text is indented
  // to line up with the list rows below (see labelIndent).
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == SHOWING_RESULT) {
    // Chapter names (remote requires the lazily-loaded Epub; local was
    // pre-computed before the Epub was released).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char remoteVal[64];
    snprintf(remoteVal, sizeof(remoteVal), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    char localVal[64];
    snprintf(localVal, sizeof(localVal), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    char deviceStr[80];
    deviceStr[0] = '\0';
    if (!remoteProgress.device.empty()) {
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
    }

    // Labeled, multi-line comparison flowing from the top. Indent everything to
    // the list rows' content-left (the row inset + side padding the list adds
    // below) so the "Remote"/"Local" labels sit directly above the row icons.
    auto labelStyle = screen.theme().bodyText;
    labelStyle.bold = true;
    auto detailStyle = screen.theme().smallText;
    const int16_t labelH = screen.target().lineHeight(labelStyle.font);
    const int16_t detailH = screen.target().lineHeight(detailStyle.font);
    const int16_t labelIndent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
    const int16_t detailIndent = static_cast<int16_t>(labelIndent + screen.theme().spaceMd);
    const auto textLine = [&](const char* text, const fui::TextStyle& style, int16_t height, int16_t indent,
                              int16_t gap) {
      fui::Rect r = screen.takeTop(height, gap);
      r.x = static_cast<int16_t>(r.x + indent);
      r.width = static_cast<int16_t>(r.width - indent);
      screen.target().text(r, text, style);
    };
    const auto labelLine = [&](const char* text) {
      textLine(text, labelStyle, labelH, labelIndent, screen.theme().spaceSm);
    };
    const auto detailLine = [&](const char* text) {
      textLine(text, detailStyle, detailH, detailIndent, screen.theme().spaceXs);
    };

    labelLine(tr(STR_REMOTE_LABEL));
    detailLine(remoteChapter.c_str());
    detailLine(remoteVal);
    if (deviceStr[0] != '\0') detailLine(deviceStr);
    screen.spacer(screen.theme().spaceLg);
    labelLine(tr(STR_LOCAL_LABEL));
    detailLine(localChapter.c_str());
    detailLine(localVal);

    // Two themed action rows flowing directly below the labels (not anchored to
    // the bottom). Rendered through the list component so they inherit the
    // active theme's row radius, insets, and selection style, matching every
    // other selectable list in the UI. Apply Remote pulls (download), Upload
    // Local pushes (upload); the selected row highlights for physical-button
    // users and tap works either way.
    screen.spacer(screen.theme().spaceMd);
    fui::ListItem actions[2];
    actions[0].label = automaticPull() ? tr(STR_RESUME) : tr(STR_APPLY_REMOTE);
    actions[0].icon = fui::bitmapFromIcon(icon_download_24);
    actions[0].actionValue = 0;
    actions[1].label = automaticPull() ? tr(STR_NO) : tr(STR_UPLOAD_LOCAL);
    actions[1].icon = fui::bitmapFromIcon(icon_upload_24);
    actions[1].actionValue = 1;
    fui::ListProps actionProps;
    actionProps.items = actions;
    actionProps.count = 2;
    actionProps.selectedIndex = static_cast<int16_t>(selectedOption);
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;  // physical buttons stay in loop()
    actionProps.scrollIndicator = false;      // never scrolls; no indicator needed
    // Non-touch hardware (X3/X4) keeps the original, denser row height instead
    // of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport); actionsBand must use the same value
    // or the band and the rows it contains fall out of sync.
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    // Keep the theme's row inset + side padding so the selected-row highlight has
    // the same padding around its icon/label as every other list in the UI; the
    // labels above are indented to match this content-left.
    const auto actionsBand =
        static_cast<int16_t>(actionRowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
    screen.list(actionProps, actionsBand);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);

    // Single themed action row anchored to the bottom, matching the lists used
    // everywhere else (inherits the theme's row radius + selection style).
    fui::ListItem action;
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    fui::ListProps actionProps;
    actionProps.items = &action;
    actionProps.count = 1;
    actionProps.selectedIndex = 0;
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    // See the equivalent override above; keeps actionsBand in sync with the
    // row height actually used on non-touch hardware (X3/X4).
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    const auto actionsBand = static_cast<int16_t>(actionRowHeight + screen.theme().spaceMd);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  // Automatic checks preserve the existing e-ink page unless there is an
  // actionable remote-ahead result. The render task still completes normally,
  // so requestUpdateAndWait() remains safe without sending pixels to the panel.
  if (automaticMode() && state != SHOWING_RESULT) {
    return;
  }

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 state == SHOWING_RESULT ? tr(STR_PROGRESS_FOUND) : tr(STR_KOREADER_SYNC));

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
    // Comparison rows + option selection render through the FreeInkApp
    // (themed rows, tap-flash); the header above shows "Progress Found".
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Prompt text + upload button render through the FreeInkApp.
    renderUi();

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
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      completeFlow();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      completeFlow();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Touch goes through the FreeInkApp: render() registered the compare rows;
    // route the snapshot and let onResultRow apply/upload on tap.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow

    // Navigate the two options with physical buttons.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseResultOption();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      completeFlow();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Touch goes through the FreeInkApp: render() registered the upload button.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow -> startUpload

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      completeFlow();
    }
    return;
  }
}
