#include "HighlightSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "KoofrCredentialStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/KoofrClient.h"
#include "util/HighlightStore.h"

namespace {
// Upper bound on files queued in one run. The queue is built before WiFi comes
// up and stays alive for the whole TLS session, so it competes with the
// handshake for heap: each entry costs two std::strings (~80 bytes at typical
// book-title lengths), capping the queue at ~8 KB. A folder larger than this
// uploads its first MAX_FILES entries; the rest go on the next run.
constexpr size_t MAX_FILES = 100;

bool hasMarkdownExtension(const char* name) {
  const size_t len = strlen(name);
  return len > 3 && strcasecmp(name + len - 3, ".md") == 0;
}
}  // namespace

size_t HighlightSyncActivity::collectHighlightFiles() {
  files.clear();
  files.reserve(16);

  // Per-book mode writes into /Highlights; enumerate whatever is there
  // regardless of the current setting, so switching modes never strands files.
  auto dir = Storage.open(HighlightStore::HIGHLIGHTS_DIR);
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    char name[128];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (files.size() >= MAX_FILES) {
        LOG_ERR("HLSYNC", "More than %u highlight files; uploading the first %u", (unsigned)MAX_FILES,
                (unsigned)MAX_FILES);
        break;
      }
      entry.getName(name, sizeof(name));
      // Skip macOS metadata files (AppleDouble resource forks)
      if (entry.isDirectory() || strncmp(name, "._", 2) == 0 || !hasMarkdownExtension(name)) continue;

      files.push_back(HighlightFile{std::string(HighlightStore::HIGHLIGHTS_DIR) + "/" + name, name});
    }
  }

  // Single-file mode writes to /Highlights.md instead.
  if (Storage.exists(HighlightStore::SINGLE_FILE_PATH)) {
    // SINGLE_FILE_PATH is "/Highlights.md"; the remote name drops the slash.
    files.push_back(HighlightFile{HighlightStore::SINGLE_FILE_PATH, HighlightStore::SINGLE_FILE_PATH + 1});
  }

  std::sort(files.begin(), files.end(), [](const HighlightFile& a, const HighlightFile& b) { return a.name < b.name; });

  LOG_DBG("HLSYNC", "Found %u highlight files", (unsigned)files.size());
  return files.size();
}

void HighlightSyncActivity::setStatus(const State newState, std::string message) {
  {
    RenderLock lock(*this);
    state = newState;
    statusMessage = std::move(message);
  }
  requestUpdate(true);
}

void HighlightSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    setStatus(FAILED, tr(STR_WIFI_CONN_FAILED));
    return;
  }

  performUpload();
}

void HighlightSyncActivity::performUpload() {
  setStatus(UPLOADING, tr(STR_KOOFR_CONNECTING));

  // Heap-allocated: KoofrClient embeds a SecureHttpClient, which is far past
  // the stack budget for a local.
  auto client = makeUniqueNoThrow<KoofrClient>();
  if (!client) {
    LOG_ERR("HLSYNC", "OOM: KoofrClient");
    setStatus(FAILED, tr(STR_LOW_MEMORY_RETRY));
    return;
  }

  const KoofrClient::Error dirError = client->ensureRemoteDir();
  if (dirError != KoofrClient::OK) {
    LOG_ERR("HLSYNC", "Remote folder failed: %d (http %d)", dirError, client->lastHttpCode);
    setStatus(FAILED, KoofrClient::errorString(dirError));
    return;
  }

  KoofrClient::Error lastError = KoofrClient::OK;
  for (size_t i = 0; i < files.size(); ++i) {
    char progress[64];
    snprintf(progress, sizeof(progress), tr(STR_KOOFR_UPLOADING_FORMAT), (int)(i + 1), (int)files.size());
    setStatus(UPLOADING, progress);

    const KoofrClient::Error error = client->uploadFile(files[i].path.c_str(), files[i].name);
    if (error == KoofrClient::OK) {
      uploadedCount++;
      continue;
    }

    // One bad file (too large, unreadable) must not abandon the rest, but an
    // auth or network failure will hit every remaining file the same way.
    failedCount++;
    lastError = error;
    LOG_ERR("HLSYNC", "Upload failed for %s: %d (http %d)", files[i].name.c_str(), error, client->lastHttpCode);
    if (error == KoofrClient::AUTH_FAILED || error == KoofrClient::NETWORK_ERROR ||
        error == KoofrClient::NO_CREDENTIALS) {
      failedCount += static_cast<int>(files.size() - i - 1);
      break;
    }
  }

  if (uploadedCount == 0 && failedCount > 0) {
    setStatus(FAILED, KoofrClient::errorString(lastError));
    return;
  }

  char summary[96];
  if (failedCount > 0) {
    snprintf(summary, sizeof(summary), tr(STR_KOOFR_PARTIAL_FORMAT), uploadedCount, failedCount);
  } else {
    snprintf(summary, sizeof(summary), tr(STR_KOOFR_UPLOADED_FORMAT), uploadedCount);
  }
  setStatus(DONE, summary);
}

void HighlightSyncActivity::onEnter() {
  Activity::onEnter();

  if (!KOOFR_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Read the SD card before the radio comes up: an empty folder needs no WiFi
  // session (and no silent reboot) at all.
  if (collectHighlightFiles() == 0) {
    state = NO_HIGHLIGHTS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("HLSYNC", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void HighlightSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    // Reboot back to home to clear the heap fragmentation the WiFi/TLS session
    // leaves behind, matching the other network activities.
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void HighlightSyncActivity::loop() {
  if (state == UPLOADING) return;

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void HighlightSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOOFR_SYNC_HIGHLIGHTS));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  switch (state) {
    case NO_CREDENTIALS:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, tr(STR_KOOFR_SETUP_HINT));
      break;
    case NO_HIGHLIGHTS:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_KOOFR_NO_HIGHLIGHTS), true, EpdFontFamily::BOLD);
      break;
    case UPLOADING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
      break;
    case DONE:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_KOOFR_UPLOAD_COMPLETE), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, statusMessage.c_str());
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_KOOFR_UPLOAD_FAILED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, statusMessage.c_str());
      break;
  }

  // No hints while uploading: nothing is interactive until the transfer ends.
  if (state != UPLOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
