#include "LanguageManifestFetchActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

// static constexpr const char* MANIFEST_URL =
//     "https://raw.githubusercontent.com/"
//     "crosspoint-reader/crosspoint-reader/master/"
//     "lib/I18n/translations/manifest.json";
static constexpr const char* MANIFEST_URL =
    "https://raw.githubusercontent.com/"
    "jpirnay/crosspoint-reader/feat-minimal-languages/"
    "lib/I18n/translations/manifest.json";

static constexpr const char* MANIFEST_PATH = "/.crosspoint/languages/manifest.json";

// ---------------------------------------------------------------------------
// WiFi callback
// ---------------------------------------------------------------------------

void LanguageManifestFetchActivity::onWifiSelectionComplete(bool connected) {
  if (!connected) {
    result.isCancelled = true;
    finish();
    return;
  }
  doFetch();
}

// ---------------------------------------------------------------------------
// Fetch
// ---------------------------------------------------------------------------

void LanguageManifestFetchActivity::doFetch() {
  {
    RenderLock lock(*this);
    _state = FETCHING;
  }
  requestUpdateAndWait();

  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/languages");

  const auto err = HttpDownloader::downloadToFile(MANIFEST_URL, MANIFEST_PATH);

  if (err != HttpDownloader::OK) {
    LOG_ERR("LANG_MF", "Manifest fetch failed: %d", static_cast<int>(err));
    {
      RenderLock lock(*this);
      _state = FAILED;
    }
    requestUpdate();
    return;
  }

  LOG_DBG("LANG_MF", "Manifest saved to %s", MANIFEST_PATH);
  {
    RenderLock lock(*this);
    _state = DONE;
  }
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Activity lifecycle
// ---------------------------------------------------------------------------

void LanguageManifestFetchActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& res) { onWifiSelectionComplete(!res.isCancelled); });
}

void LanguageManifestFetchActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void LanguageManifestFetchActivity::loop() {
  if (_state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      result.isCancelled = true;
      finish();
    }
    return;
  }

  if (_state == DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();  // isCancelled = false → LanguageSelectActivity will refresh list
    }
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void LanguageManifestFetchActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = (pageHeight - lineHeight) / 2;

  if (_state == FETCHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DOWNLOADING));

  } else if (_state == DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DONE), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (_state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
