#include "OpdsStartupSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsTokenStore.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CANCEL = 1;
constexpr int PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 5000;
}  // namespace

OpdsStartupSyncActivity::OpdsStartupSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("OpdsStartupSync", renderer, mappedInput), UiAppHost(renderer) {}

bool OpdsStartupSyncActivity::shouldRun() {
  return SETTINGS.opdsSyncOnStartup != 0 && OPDS_STORE.getCount() > 0 && WIFI_STORE.getCredentialCount() > 0;
}

void OpdsStartupSyncActivity::onEnter() {
  Activity::onEnter();

  state = SyncState::CONNECTING;
  cancelled = false;
  status = {};
  statusMessage = tr(STR_CONNECTING);

  resetUi();
  app.on(ACTION_CANCEL, &OpdsStartupSyncActivity::onCancelEvent, this);
  app.setScreen(&OpdsStartupSyncActivity::rootScreen, this);
  requestUpdate();

  const auto* first = OPDS_STORE.getServer(0);
  if (!first) {
    onGoHome();
    return;
  }
  server = *first;  // Copied: the sync outlives any store reload
  if (!startWifi()) {
    onGoHome();
    return;
  }
  connectStartedAt = millis();
}

// Joins the last connected network, or the first saved one. A single attempt:
// scanning and retrying belongs to WifiSelectionActivity, not to a boot path.
bool OpdsStartupSyncActivity::startWifi() {
  auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) credential = WIFI_STORE.getCredentialAt(0);
  if (!credential) return false;

  LOG_INF("OPDS", "Startup sync: joining %s", credential->ssid.c_str());
  WiFi.mode(WIFI_STA);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  return true;
}

void OpdsStartupSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    // Same teardown as the OPDS browser: drop the station and reboot silently so
    // the reader starts from an unfragmented heap. The reboot lands on home and
    // is itself skipped by the startup-sync check in setup().
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsStartupSyncActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsStartupSyncActivity*>(user);
  self->app.clearTapFlash();
  self->cancelled = true;
}

void OpdsStartupSyncActivity::loop() {
  if (state == SyncState::SYNCING) return;  // runSync() owns the loop while it runs

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelled = true;
  const auto route = routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  if (cancelled) {
    LOG_INF("OPDS", "Startup sync cancelled");
    onGoHome();
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = SyncState::SYNCING;
    statusMessage.clear();
    requestUpdate(true);
    runSync();
    onGoHome();
    return;
  }

  if (millis() - connectStartedAt > CONNECT_TIMEOUT_MS) {
    LOG_INF("OPDS", "Startup sync: no WiFi within %lu ms, booting on", CONNECT_TIMEOUT_MS);
    onGoHome();
  }
}

void OpdsStartupSyncActivity::runSync() {
  renderedPercent = -1;
  lastProgressUpdateMs = 0;
  opdsClient.setServer(server.url, server.username, server.password, server.activeCustomHeaders());
  const std::string tokenKey = server.url + '\x1f' + server.username;
  {
    const OpdsTokens t = OPDS_TOKENS.get(tokenKey);
    opdsClient.setTokens(t.accessToken, t.refreshToken, t.refreshUrl);
  }
  opdsClient.resetAuthState();
  const OpdsBatchDownload::Hooks hooks{this, &OpdsStartupSyncActivity::onProgress,
                                       &OpdsStartupSyncActivity::onTransfer};
  const auto result = OpdsBatchDownload::run(opdsClient, server.url, hooks, status);
  if (opdsClient.tokensDirty()) {
    OpdsTokens tokens;
    tokens.accessToken = opdsClient.accessToken();
    tokens.refreshToken = opdsClient.refreshToken();
    tokens.refreshUrl = opdsClient.refreshUrl();
    if (!OPDS_TOKENS.put(tokenKey, tokens)) LOG_ERR("OPDS", "Failed to persist OPDS tokens for %s", server.url.c_str());
  }
  if (result == OpdsBatchDownload::Result::LIMIT_REACHED) LOG_INF("OPDS", "Startup sync stopped at its limit");
  LOG_INF("OPDS", "Startup sync: %d new, %d skipped, %d failed of %d (result %d)", status.downloaded, status.skipped,
          status.failed, status.examined, static_cast<int>(result));
}

bool OpdsStartupSyncActivity::onProgress(void* ctx, const OpdsBatchDownload::Status& progress) {
  auto* self = static_cast<OpdsStartupSyncActivity*>(ctx);
  self->statusMessage = progress.title;
  self->bytes = self->bytesTotal = 0;
  self->pumpProgress();
  return !self->cancelled;
}

HttpDownloader::DownloadError OpdsStartupSyncActivity::onTransfer(void* ctx, const std::string& url,
                                                                  const std::string& dest,
                                                                  const freeink::opds::HttpAuth& auth) {
  auto* self = static_cast<OpdsStartupSyncActivity*>(ctx);
  std::vector<HttpDownloader::Header> headers = auth.headers;
  if (!auth.bearer.empty()) headers.push_back({"Authorization", "Bearer " + auth.bearer});
  return HttpDownloader::downloadToFile(
      url, dest,
      [self](const size_t downloaded, const size_t total) {
        self->bytes = downloaded;
        self->bytesTotal = total;
        self->pumpProgress();
      },
      &self->cancelled, auth.username, auth.password, headers);
}

void OpdsStartupSyncActivity::pumpProgress() {
  // The activity loop is blocked for the whole sync; pump input here so Back or
  // the Cancel button can abort between chunks.
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelled = true;
  routeTouch(mappedInput);

  const int percent = bytesTotal > 0 ? static_cast<int>(static_cast<uint64_t>(bytes) * 100 / bytesTotal) : 0;
  const unsigned long now = millis();
  if (percent >= 100 || renderedPercent < 0 || percent < renderedPercent ||
      percent >= renderedPercent + PROGRESS_STEP_PERCENT || now - lastProgressUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
    renderedPercent = percent;
    lastProgressUpdateMs = now;
    requestUpdate(true);
  }
}

void OpdsStartupSyncActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<OpdsStartupSyncActivity*>(user);
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.takeBottom(static_cast<int16_t>(metrics.buttonHintsHeight));
  screen.spacer(static_cast<int16_t>(metrics.topPadding));

  fui::HeaderProps header;
  header.title = self->server.name.empty() ? tr(STR_OPDS_BROWSER) : self->server.name.c_str();
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Centered block: state line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 3 + barH + btnH + gap * 4);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_OPDS_DOWNLOAD_ALL_NEW), centered);
  screen.target().text(screen.takeTop(lh, gap), self->statusMessage.c_str(), centered);

  char counters[24];
  snprintf(counters, sizeof(counters), "%d / %d", self->status.downloaded, self->status.examined);
  screen.target().text(screen.takeTop(lh, gap), counters, centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (self->bytesTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(self->bytes);
    progress.max = static_cast<int32_t>(self->bytesTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsStartupSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderUi();
  renderer.displayBuffer();
}
