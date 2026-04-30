#include "TextBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HtmlToText.h"
#include "network/HttpDownloader.h"

namespace {
// Font used for body text
constexpr int BODY_FONT = NOTOSANS_12_FONT_ID;
// Left/right margin for content text (pixels)
constexpr int SIDE_MARGIN = 20;
// Height reserved at the top for the title header bar
constexpr int HEADER_H = 32;
// Height reserved at the bottom for button hints
constexpr int FOOTER_H = 40;
// Vertical offset of text baseline within the header bar
constexpr int HEADER_TEXT_Y = 18;
// Maximum wrapped lines considered per input paragraph (safety cap)
constexpr int MAX_LINES_PER_PARA = 200;
}  // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void TextBrowserActivity::onEnter() {
  Activity::onEnter();
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();
  checkAndConnectWifi();
}

void TextBrowserActivity::onExit() {
  lines.clear();
  lines.shrink_to_fit();
  urlHistory.clear();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

void TextBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    launchUrlInput();
    return;
  }
  launchWifiSelection();
}

void TextBrowserActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void TextBrowserActivity::onWifiSelectionComplete(bool connected) {
  if (connected) {
    launchUrlInput();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

// ── URL input ─────────────────────────────────────────────────────────────────

void TextBrowserActivity::launchUrlInput(const std::string& initial) {
  consumeConfirm = true;
  state = State::URL_INPUT;
  requestUpdate();

  auto kb = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_URL), initial, 0,
                                                    InputType::Url);
  startActivityForResult(std::move(kb), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      if (urlHistory.empty()) {
        onGoHome();
      } else {
        state = State::BROWSING;
        requestUpdate();
      }
      return;
    }
    const auto& text = std::get<KeyboardResult>(result.data).text;
    if (!text.empty()) {
      fetchAndProcess(text);
    } else {
      state = urlHistory.empty() ? State::ERROR : State::BROWSING;
      if (state == State::ERROR) errorMessage = tr(STR_ENTER_URL);
      requestUpdate();
    }
  });
}

// ── Fetch & convert ───────────────────────────────────────────────────────────

void TextBrowserActivity::fetchAndProcess(const std::string& url) {
  currentUrl = url;
  state = State::LOADING;
  statusMessage = tr(STR_FETCHING_PAGE);
  // Immediate render so the user sees "Fetching…" before the blocking HTTP call
  requestUpdate(true);

  LOG_DBG("BROWSER", "Fetching: %s", url.c_str());

  std::string html;
  if (!HttpDownloader::fetchUrl(url, html)) {
    LOG_ERR("BROWSER", "Fetch failed: %s", url.c_str());
    state = State::ERROR;
    errorMessage = tr(STR_FETCH_URL_FAILED);
    requestUpdate();
    return;
  }

  LOG_DBG("BROWSER", "Received %u bytes", static_cast<unsigned>(html.size()));

  pageTitle = HtmlToText::extractTitle(html);
  std::string text = HtmlToText::convert(html, MAX_HTML_BYTES);

  // Release raw HTML immediately to reclaim heap before the lines vector grows
  html.clear();
  html.shrink_to_fit();

  if (text.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_NO_CONTENT);
    requestUpdate();
    return;
  }

  // Add to history (dedup tip)
  if (urlHistory.empty() || urlHistory.back() != url) {
    if (static_cast<int>(urlHistory.size()) >= MAX_HISTORY) {
      urlHistory.erase(urlHistory.begin());
    }
    urlHistory.push_back(url);
  }

  buildPages(text);

  if (lines.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_NO_CONTENT);
    requestUpdate();
    return;
  }

  currentPage = 0;
  state = State::BROWSING;
  requestUpdate();
}

// ── Page building ─────────────────────────────────────────────────────────────

void TextBrowserActivity::buildPages(const std::string& text) {
  lines.clear();

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int maxLineW = screenW - 2 * SIDE_MARGIN;
  const int lineH = renderer.getLineHeight(BODY_FONT);
  const int contentH = screenH - HEADER_H - FOOTER_H;

  linesPerPage = (lineH > 0) ? (contentH / lineH) : 20;
  if (linesPerPage < 1) linesPerPage = 1;

  // Conservative estimate: ~300 lines typical for a capped 64 KB page
  lines.reserve(300);

  // Walk the plain-text output and wrap each paragraph individually.
  // renderer.wrappedText() only reads const font tables — safe from any task.
  size_t pos = 0;
  const size_t textLen = text.size();

  while (pos <= textLen) {
    const size_t nlPos = (pos < textLen) ? text.find('\n', pos) : textLen;
    const size_t paraEnd = (nlPos == std::string::npos) ? textLen : nlPos;
    const size_t paraLen = paraEnd - pos;

    if (paraLen == 0) {
      // Blank line — preserve paragraph spacing
      lines.emplace_back();
    } else {
      // Build a null-terminated copy of this paragraph for the renderer
      std::string para(text.data() + pos, paraLen);
      auto wrapped = renderer.wrappedText(BODY_FONT, para.c_str(), maxLineW, MAX_LINES_PER_PARA);
      for (auto& line : wrapped) {
        lines.push_back(std::move(line));
      }
    }

    pos = paraEnd + 1;
  }

  totalPages = (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage;
  if (totalPages < 1) totalPages = 1;

  LOG_DBG("BROWSER", "Built %d lines, %d pages (%d lines/page)", static_cast<int>(lines.size()), totalPages,
          linesPerPage);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void TextBrowserActivity::loop() {
  if (state == State::WIFI_SELECTION || state == State::URL_INPUT) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onGoHome();
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchUrlInput(urlHistory.empty() ? "https://" : urlHistory.back());
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchUrlInput(currentUrl);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (urlHistory.size() > 1) {
        // Go back to the previous URL
        urlHistory.pop_back();
        const std::string prev = urlHistory.back();
        urlHistory.pop_back();  // fetchAndProcess will re-add it
        fetchAndProcess(prev);
      } else {
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
        onGoHome();
      }
      return;
    }

    // Up/Left → previous page, Down/Right → next page
    buttonNavigator.onPreviousRelease([this] {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextRelease([this] {
      if (currentPage < totalPages - 1) {
        currentPage++;
        requestUpdate();
      }
    });
    buttonNavigator.onPreviousContinuous([this] {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextContinuous([this] {
      if (currentPage < totalPages - 1) {
        currentPage++;
        requestUpdate();
      }
    });
  }
}

// ── Render ────────────────────────────────────────────────────────────────────

void TextBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // ── Title header bar ──────────────────────────────────────────────────────
  {
    const char* rawTitle = pageTitle.empty() ? tr(STR_WEB_BROWSER) : pageTitle.c_str();

    if (state == State::BROWSING) {
      // Left: page title (truncated); Right: page count
      char pageBuf[16];
      snprintf(pageBuf, sizeof(pageBuf), "%d/%d", currentPage + 1, totalPages);
      const int countW = renderer.getTextWidth(UI_10_FONT_ID, pageBuf) + 8;
      auto title = renderer.truncatedText(UI_10_FONT_ID, rawTitle, screenW - countW - SIDE_MARGIN);
      renderer.drawText(UI_10_FONT_ID, SIDE_MARGIN, HEADER_TEXT_Y, title.c_str(), true);
      renderer.drawText(UI_10_FONT_ID, screenW - countW, HEADER_TEXT_Y, pageBuf, true);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, HEADER_TEXT_Y, rawTitle, true);
    }
    renderer.drawLine(0, HEADER_H - 1, screenW - 1, HEADER_H - 1, true);
  }

  // ── Loading / WiFi check ──────────────────────────────────────────────────
  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, screenH / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ── Error ─────────────────────────────────────────────────────────────────
  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, screenH / 2 - 14, tr(STR_ERROR_MSG));
    auto truncErr = renderer.truncatedText(UI_10_FONT_ID, errorMessage.c_str(), screenW - 2 * SIDE_MARGIN);
    renderer.drawCenteredText(UI_10_FONT_ID, screenH / 2 + 14, truncErr.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ENTER_URL), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ── Browsing — paginated text ─────────────────────────────────────────────
  if (state == State::BROWSING) {
    const int lineH = renderer.getLineHeight(BODY_FONT);
    const int startLine = currentPage * linesPerPage;
    const int endLine = std::min<int>(startLine + linesPerPage, static_cast<int>(lines.size()));

    int y = HEADER_H + lineH;
    for (int li = startLine; li < endLine; li++) {
      if (!lines[li].empty()) {
        renderer.drawText(BODY_FONT, SIDE_MARGIN, y, lines[li].c_str(), true);
      }
      y += lineH;
    }

    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_ENTER_URL), tr(STR_PAGE_PREV), tr(STR_PAGE_NEXT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.displayBuffer();
}
