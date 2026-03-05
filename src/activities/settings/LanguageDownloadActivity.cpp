#include "LanguageDownloadActivity.h"

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

// static constexpr const char* LANG_DOWNLOAD_BASE =
//     "https://raw.githubusercontent.com/"
//     "crosspoint-reader/crosspoint-reader/master/"
//     "lib/I18n/translations/";
static constexpr const char* LANG_DOWNLOAD_BASE =
    "https://raw.githubusercontent.com/"
    "jpirnay/crosspoint-reader/feat-minimal-languages/"
    "lib/I18n/translations/";

static bool isValidLanguageCode(const std::string& code) {
  return code.size() == 2 && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z';
}

// ---------------------------------------------------------------------------
// WiFi callback
// ---------------------------------------------------------------------------

void LanguageDownloadActivity::onWifiSelectionComplete(bool connected) {
  if (!connected) {
    LOG_ERR("LANG_DL", "WiFi connection failed");
    result.isCancelled = true;
    finish();
    return;
  }

  LOG_DBG("LANG_DL", "WiFi connected, downloading %s", _code.c_str());
  doDownload();
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

void LanguageDownloadActivity::doDownload() {
  {
    RenderLock lock(*this);
    _state = DOWNLOADING;
    _progressPercent = 0;
  }
  requestUpdateAndWait();

  if (!isValidLanguageCode(_code)) {
    LOG_ERR("LANG_DL", "Invalid language code: %s", _code.c_str());
    {
      RenderLock lock(*this);
      _state = DOWNLOAD_FAILED;
    }
    requestUpdate();
    return;
  }

  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/languages");

  const std::string url = std::string(LANG_DOWNLOAD_BASE) + _code + ".yaml";
  const std::string dest = std::string("/.crosspoint/languages/") + _code + ".yaml";

  const auto err = HttpDownloader::downloadToFile(url, dest, [this](size_t downloaded, size_t total) {
    if (total == 0) return;
    const int pct = static_cast<int>((downloaded * 100) / total);
    {
      RenderLock lock(*this);
      _progressPercent = pct;
    }
    requestUpdate();
  });

  if (err != HttpDownloader::OK) {
    LOG_ERR("LANG_DL", "Download failed (%d) for %s", static_cast<int>(err), url.c_str());
    {
      RenderLock lock(*this);
      _state = DOWNLOAD_FAILED;
    }
    requestUpdate();
    return;
  }

  if (!I18N.setExternalLanguage(_code.c_str())) {
    LOG_ERR("LANG_DL", "Failed to activate downloaded language %s", _code.c_str());
    {
      RenderLock lock(*this);
      _state = DOWNLOAD_FAILED;
    }
    requestUpdate();
    return;
  }

  LOG_DBG("LANG_DL", "Language %s downloaded and activated", _code.c_str());
  {
    RenderLock lock(*this);
    _state = DONE;
  }
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Activity lifecycle
// ---------------------------------------------------------------------------

void LanguageDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& res) { onWifiSelectionComplete(!res.isCancelled); });
}

void LanguageDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void LanguageDownloadActivity::loop() {
  if (_state == DOWNLOAD_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      result.isCancelled = true;
      finish();
    }
    return;
  }

  if (_state == DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();  // isCancelled = false → LanguageSelectActivity will go back
    }
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void LanguageDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = (pageHeight - lineHeight) / 2;

  if (_state == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight - metrics.verticalSpacing, _name.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DOWNLOADING));

    const int barY = centerY + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        _progressPercent, 100);

  } else if (_state == DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, _name.c_str(), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (_state == DOWNLOAD_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
