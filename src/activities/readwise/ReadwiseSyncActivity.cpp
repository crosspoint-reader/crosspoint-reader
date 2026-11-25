#include "ReadwiseSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiPowerSaveGuard.h"
#include "readwise/ReadwiseSync.h"

ReadwiseSyncActivity::ReadwiseSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadwiseSync", renderer, mappedInput) {}

ReadwiseSyncActivity::~ReadwiseSyncActivity() = default;

void ReadwiseSyncActivity::onEnter() {
  Activity::onEnter();
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadwiseSyncActivity::onExit() {
  Activity::onExit();
  sync.reset();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ReadwiseSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }
  startSync();
}

void ReadwiseSyncActivity::startSync() {
  RenderLock lock(*this);
  sync = makeUniqueNoThrow<ReadwiseSync>();
  if (!sync) {
    state = FAILED;
    errorMessage = "Out of memory";
  } else if (!sync->begin()) {
    state = FAILED;
    errorMessage = sync->error();
  } else {
    state = RUNNING;
    lastStatus = statusLine();
  }
  requestUpdate();
}

std::string ReadwiseSyncActivity::statusLine() const {
  if (!sync) return "";
  char buf[96];
  const uint32_t waitMs = sync->waitRemainingMs();
  if (waitMs > 5000) {
    std::snprintf(buf, sizeof(buf), tr(STR_READWISE_WAITING), static_cast<int>((waitMs + 999) / 1000));
    return buf;
  }
  switch (sync->phase()) {
    case ReadwiseSync::Phase::Auth:
      return tr(STR_READWISE_CHECKING);
    case ReadwiseSync::Phase::List:
      return tr(STR_READWISE_LISTING);
    case ReadwiseSync::Phase::Progress:
    case ReadwiseSync::Phase::Cleanup:
      return tr(STR_READWISE_UPDATING);
    case ReadwiseSync::Phase::Download:
      std::snprintf(buf, sizeof(buf), tr(STR_READWISE_DOWNLOADING), std::min(sync->itemIndex() + 1, sync->itemCount()),
                    sync->itemCount());
      return buf;
    default:
      return "";
  }
}

void ReadwiseSyncActivity::loop() {
  if (state == RUNNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Stopping between steps is safe: the index is saved after every change.
      finish();
      return;
    }
    {
      WifiPowerSaveGuard psGuard;
      sync->step();
    }
    if (sync->finished()) {
      RenderLock lock(*this);
      state = sync->phase() == ReadwiseSync::Phase::Done ? DONE : FAILED;
      errorMessage = sync->error();
      requestUpdate();
      return;
    }
    // Repaint only when the visible text changes (e-ink refreshes are slow).
    std::string status = statusLine();
    const std::string title = sync->currentTitle();
    status += '\n';
    status += title;
    if (status != lastStatus) {
      RenderLock lock(*this);
      lastStatus = std::move(status);
      requestUpdate();
    }
    return;
  }

  if (state == DONE || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}

void ReadwiseSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READWISE));

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = pageHeight / 2 - lineHeight * 2;
  const int textWidth = pageWidth - 2 * metrics.contentSidePadding;

  if (state == RUNNING && sync) {
    const std::string status = statusLine();
    renderer.drawCenteredText(UI_10_FONT_ID, top, status.c_str(), true, EpdFontFamily::BOLD);
    const std::string& title = sync->currentTitle();
    if (!title.empty()) {
      const std::string shown = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), textWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, shown.c_str());
    }
  } else if (state == DONE && sync) {
    const auto& s = sync->stats();
    char line[96];
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_READWISE_DONE), true, EpdFontFamily::BOLD);
    std::snprintf(line, sizeof(line), tr(STR_READWISE_SUMMARY), s.downloaded, s.archived, s.removed);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, line);
    if (s.progress || s.skipped || s.failed) {
      std::snprintf(line, sizeof(line), tr(STR_READWISE_SUMMARY_MORE), s.progress, s.skipped, s.failed);
      renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * (lineHeight + 10), line);
    }
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_READWISE_FAILED), true, EpdFontFamily::BOLD);
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, errorMessage.c_str(), textWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, shown.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
