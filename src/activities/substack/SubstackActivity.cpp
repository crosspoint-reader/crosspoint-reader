#include "SubstackActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cctype>
#include <cstring>

#include "SubstackStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"

#ifndef CROSSSTACK_DEFAULT_API_URL
#define CROSSSTACK_DEFAULT_API_URL ""
#endif

namespace {
// Percent-encodes `input` (query-param use only — SubstackActivity's own
// device_name, never user-controlled) into a stack buffer. HttpDownloader
// has no POST support (see HttpDownloader.h), so device_name travels as a
// GET query param instead of a JSON body — see db/migrate.ts's
// device_pairings comment on the API side for the same reasoning.
void urlEncodeInto(const char* input, char* out, size_t outLen) {
  size_t pos = 0;
  for (const auto* p = reinterpret_cast<const unsigned char*>(input); *p && pos + 4 < outLen; ++p) {
    if (std::isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      out[pos++] = static_cast<char>(*p);
    } else {
      pos += static_cast<size_t>(snprintf(out + pos, outLen - pos, "%%%02X", *p));
    }
  }
  out[pos] = '\0';
}
}  // namespace

// ── URL helpers ───────────────────────────────────────────────────────────────

void SubstackActivity::buildPendingUrl(char* buf, size_t bufLen) const {
  snprintf(buf, bufLen, "%s/device/%s/pending", SUBSTACK_STORE.apiUrl, SUBSTACK_STORE.deviceId);
}

void SubstackActivity::buildEpubUrl(char* buf, size_t bufLen, const char* epubId) const {
  snprintf(buf, bufLen, "%s/device/%s/epub/%s", SUBSTACK_STORE.apiUrl, SUBSTACK_STORE.deviceId, epubId);
}

// ── JSON parser ───────────────────────────────────────────────────────────────

bool SubstackActivity::parsePendingResponse(const char* json, size_t /*len*/) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("SUBSTACK", "JSON parse error");
    return false;
  }
  if (!doc.is<JsonArray>()) {
    LOG_ERR("SUBSTACK", "Expected JSON array from pending endpoint");
    return false;
  }

  epubCount = 0;
  for (JsonObjectConst item : doc.as<JsonArray>()) {
    if (epubCount >= MAX_EPUBS) break;
    SubstackEpub& e = epubs[epubCount];
    snprintf(e.id,          sizeof(e.id),          "%s", item["id"]          | "");
    snprintf(e.title,       sizeof(e.title),        "%s", item["title"]       | "");
    snprintf(e.profileName, sizeof(e.profileName),  "%s", item["profileName"] | "");
    e.epubSize = static_cast<size_t>(item["epubSize"] | 0);
    ++epubCount;
  }
  return true;
}

// ── network task ──────────────────────────────────────────────────────────────

void SubstackActivity::networkTaskTrampoline(void* param) {
  static_cast<SubstackActivity*>(param)->runNetworkTask();
  vTaskDelete(nullptr);
}

void SubstackActivity::runNetworkTask() {
  char url[256];
  buildPendingUrl(url, sizeof(url));

  std::string response;
  // HttpDownloader supports Basic auth; pass "token" as username and the JWT as
  // password — the CrossInk API accepts both Basic and Bearer.
  bool ok = HttpDownloader::fetchUrl(url, response, "token", SUBSTACK_STORE.apiToken);
  if (!ok) {
    LOG_ERR("SUBSTACK", "Failed to fetch pending list from %s", url);
    snprintf(errorMsg, sizeof(errorMsg), "%s", tr(STR_SUBSTACK_FETCH_ERROR));
    state = State::ERROR;
    activityManager.requestUpdate();
    return;
  }

  if (!parsePendingResponse(response.c_str(), response.size())) {
    snprintf(errorMsg, sizeof(errorMsg), "%s", tr(STR_SUBSTACK_PARSE_ERROR));
    state = State::ERROR;
    activityManager.requestUpdate();
    return;
  }

  state = (epubCount > 0) ? State::LIST : State::EMPTY;
  activityManager.requestUpdate();
}

// ── pairing (no account context yet) ─────────────────────────────────────────

bool SubstackActivity::requestPairingCode() {
  const char* apiUrl = SUBSTACK_STORE.apiUrl[0] ? SUBSTACK_STORE.apiUrl : CROSSSTACK_DEFAULT_API_URL;
  snprintf(pairApiUrl, sizeof(pairApiUrl), "%s", apiUrl);

  char encodedName[96];
  urlEncodeInto(BoardConfig::ACTIVE.name, encodedName, sizeof(encodedName));

  char url[256];
  snprintf(url, sizeof(url), "%s/pair/new?device_name=%s", pairApiUrl, encodedName);

  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    LOG_ERR("SUBSTACK", "Failed to request a pairing code from %s", url);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    LOG_ERR("SUBSTACK", "Pairing response JSON parse error");
    return false;
  }
  snprintf(pairCode, sizeof(pairCode), "%s", doc["code"] | "");
  snprintf(pairUrl, sizeof(pairUrl), "%s", doc["pair_url"] | "");
  return pairCode[0] != '\0' && pairUrl[0] != '\0';
}

void SubstackActivity::pairingTaskTrampoline(void* param) {
  static_cast<SubstackActivity*>(param)->runPairingTask();
  vTaskDelete(nullptr);
}

void SubstackActivity::runPairingTask() {
  if (!requestPairingCode()) {
    snprintf(errorMsg, sizeof(errorMsg), "%s", tr(STR_SUBSTACK_FETCH_ERROR));
    state = State::ERROR;
    activityManager.requestUpdate();
    return;
  }
  // pairCode/pairUrl are set now — redraw so the QR actually appears
  // (the first render, right after onEnter, had neither yet).
  activityManager.requestUpdate();

  constexpr int kPollIntervalMs = 3000;
  constexpr int kPollSliceMs = 200;
  char pollUrl[256];
  snprintf(pollUrl, sizeof(pollUrl), "%s/pair/%s", pairApiUrl, pairCode);

  while (!pairingCancelled) {
    std::string response;
    if (HttpDownloader::fetchUrl(pollUrl, response)) {
      JsonDocument doc;
      if (deserializeJson(doc, response) == DeserializationError::Ok &&
          strcmp(doc["status"] | "", "claimed") == 0) {
        // Persist before switching state: if a power loss hits right after,
        // isConfigured() is still true on the next boot and FETCHING resumes
        // normally instead of re-pairing.
        snprintf(SUBSTACK_STORE.apiUrl, sizeof(SUBSTACK_STORE.apiUrl), "%s", pairApiUrl);
        snprintf(SUBSTACK_STORE.deviceId, sizeof(SUBSTACK_STORE.deviceId), "%s", doc["device_id"] | "");
        snprintf(SUBSTACK_STORE.apiToken, sizeof(SUBSTACK_STORE.apiToken), "%s", doc["token"] | "");
        SUBSTACK_STORE.saveToFile();

        state = State::FETCHING;
        activityManager.requestUpdate();
        xTaskCreate(&networkTaskTrampoline, "SubstackFetch", 4096, this, 1, &networkTask);
        return;
      }
    }
    for (int waited = 0; waited < kPollIntervalMs && !pairingCancelled; waited += kPollSliceMs) {
      vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
    }
  }
}

// ── Activity lifecycle ────────────────────────────────────────────────────────

void SubstackActivity::onEnter() {
  Activity::onEnter();
  pairingCancelled = false;

  SUBSTACK_STORE.loadFromFile();

  if (!SUBSTACK_STORE.isConfigured()) {
    state = State::PAIRING;
    pairCode[0] = '\0';
    pairUrl[0] = '\0';
    requestUpdate();
    // Requesting the code and then polling both do blocking HTTP I/O, so
    // both run on the background task — same reasoning as runNetworkTask
    // below, just started one step earlier.
    xTaskCreate(&pairingTaskTrampoline, "SubstackPair", 4096, this, 1, &networkTask);
    return;
  }

  state = State::FETCHING;
  requestUpdate();

  // Network I/O on a background task — 4KB stack (HTTP client needs headroom)
  xTaskCreate(&networkTaskTrampoline, "SubstackFetch", 4096, this, 1, &networkTask);
}

void SubstackActivity::onExit() {
  // Let the pairing poll's sleep loop (if running) exit on its next 200ms
  // slice instead of running out the clock on the wait below.
  pairingCancelled = true;

  // Wait up to 2 s for the fetch task to finish before destroying this Activity,
  // since the task holds a pointer to our epubs[] and errorMsg buffers.
  if (networkTask) {
    constexpr TickType_t kMaxWait = pdMS_TO_TICKS(2000);
    const TickType_t deadline = xTaskGetTickCount() + kMaxWait;
    while (eTaskGetState(networkTask) != eDeleted && xTaskGetTickCount() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    networkTask = nullptr;
  }
  Activity::onExit();
}

// ── loop ──────────────────────────────────────────────────────────────────────

void SubstackActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::SUBSTACK);
    return;
  }

  if (state != State::LIST) return;

  const int count = static_cast<int>(epubCount);

  buttonNavigator.onNext([this, count] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
    return;
  }

  // Row touch — use the same grid metrics as drawButtonMenu
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int menuTop = metrics.headerHeight;
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  int menuRow = -1;
  const auto touch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing,
                                          count, 0, INT32_MAX, menuRowHeight);
  if (touch != MappedInputManager::RowTouch::None) {
    if (touch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != menuRow) {
        selectorIndex = menuRow;
        requestUpdate();
      }
    } else {
      selectorIndex = menuRow;
      openSelected();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
  }
}

void SubstackActivity::openSelected() {
  if (selectorIndex < 0 || static_cast<size_t>(selectorIndex) >= epubCount) return;
  char epubPath[192];
  snprintf(epubPath, sizeof(epubPath), "/substacks/%s.epub", epubs[selectorIndex].id);
  if (Storage.exists(epubPath)) {
    activityManager.goToReader(epubPath);
  } else {
    // File not yet on SD — show downloading state and kick off the download
    state = State::DOWNLOADING;
    downloadProgress = 0;
    requestUpdate();
    xTaskCreate(&downloadTaskTrampoline, "SubstackDL", 4096, this, 1, &networkTask);
  }
}

void SubstackActivity::downloadTaskTrampoline(void* param) {
  static_cast<SubstackActivity*>(param)->runDownloadTask();
  vTaskDelete(nullptr);
}

void SubstackActivity::runDownloadTask() {
  if (selectorIndex < 0 || static_cast<size_t>(selectorIndex) >= epubCount) return;
  const SubstackEpub& epub = epubs[selectorIndex];

  char url[256];
  buildEpubUrl(url, sizeof(url), epub.id);

  // Ensure destination directory exists
  Storage.mkdir("/substacks");

  char destPath[192];
  snprintf(destPath, sizeof(destPath), "/substacks/%s.epub", epub.id);

  const auto progressCb = [this](size_t downloaded, size_t total) {
    downloadProgress = (total > 0) ? static_cast<int>((downloaded * 100) / total) : 0;
    activityManager.requestUpdate();
  };

  const auto err = HttpDownloader::downloadToFile(url, destPath, progressCb, nullptr,
                                                  "token", SUBSTACK_STORE.apiToken);
  if (err != HttpDownloader::OK) {
    LOG_ERR("SUBSTACK", "Download failed for epub %s (err=%d)", epub.id, static_cast<int>(err));
    snprintf(errorMsg, sizeof(errorMsg), "%s", tr(STR_SUBSTACK_DOWNLOAD_ERROR));
    state = State::ERROR;
    networkTask = nullptr;
    activityManager.requestUpdate();
    return;
  }

  networkTask = nullptr;
  // Open reader now that the file is on SD
  char epubPath[192];
  snprintf(epubPath, sizeof(epubPath), "/substacks/%s.epub", epub.id);
  activityManager.goToReader(epubPath);
}

// ── render ────────────────────────────────────────────────────────────────────

void SubstackActivity::render(RenderLock&&) {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics   = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight - metrics.topPadding},
                 tr(STR_SUBSTACKS));

  const int contentTop    = metrics.headerHeight;
  const int contentHeight = pageHeight - metrics.headerHeight - metrics.buttonHintsHeight;

  // Longest of these (STR_SUBSTACK_FETCH_ERROR/STR_SUBSTACK_NO_EPUBS) can
  // wrap on the panel width, so this uses the same
  // UITheme::drawCenteredWrappedText(renderer, bounds, fontId, text, maxLines)
  // pattern as other activities' status messages (e.g. WifiSelectionActivity,
  // SdFirmwareUpdateActivity) rather than a plain single-line draw.
  constexpr int MAX_STATUS_LINES = 3;

  switch (state) {
    case State::PAIRING: {
      if (!pairCode[0]) {
        // Still waiting on the GET /pair/new response — nothing to show a
        // QR for yet.
        UITheme::drawCenteredWrappedText(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                                         UI_12_FONT_ID, tr(STR_LOADING_POPUP), MAX_STATUS_LINES);
        break;
      }
      constexpr int kCodeRowHeight = 30;
      const int qrHeight = contentHeight - kCodeRowHeight;
      QrUtils::drawQrCode(renderer, Rect{0, contentTop, pageWidth, qrHeight}, pairUrl);

      char codeLine[64];
      snprintf(codeLine, sizeof(codeLine), "%s: %s", tr(STR_SUBSTACK_PAIRING_CODE), pairCode);
      const int codeY = contentTop + qrHeight + (kCodeRowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      UITheme::drawCenteredText(renderer, Rect{0, contentTop + qrHeight, pageWidth, kCodeRowHeight}, UI_10_FONT_ID,
                                codeY, codeLine);
      break;
    }

    case State::FETCHING:
      UITheme::drawCenteredWrappedText(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                                       UI_12_FONT_ID, tr(STR_LOADING_POPUP), MAX_STATUS_LINES);
      break;

    case State::EMPTY:
      UITheme::drawCenteredWrappedText(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                                       UI_12_FONT_ID, tr(STR_SUBSTACK_NO_EPUBS), MAX_STATUS_LINES);
      break;

    case State::ERROR:
      UITheme::drawCenteredWrappedText(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                                       UI_12_FONT_ID, errorMsg, MAX_STATUS_LINES);
      break;

    case State::DOWNLOADING: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s %d%%", tr(STR_SUBSTACK_DOWNLOADING), downloadProgress);
      UITheme::drawCenteredWrappedText(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                                       UI_12_FONT_ID, buf, MAX_STATUS_LINES);
      break;
    }

    case State::LIST: {
      const int count = static_cast<int>(epubCount);
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectorIndex,
          [this](int i) {
            // Build "Title — Newsletter" using a stack buffer to avoid heap allocation
            char buf[160];
            snprintf(buf, sizeof(buf), "%s \xe2\x80\x94 %s", epubs[i].title, epubs[i].profileName);
            return std::string(buf);
          },
          [](int /*i*/) { return UIIcon::Book; });
      break;
    }
  }

  const auto labels =
      (state == State::LIST)
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
          : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
