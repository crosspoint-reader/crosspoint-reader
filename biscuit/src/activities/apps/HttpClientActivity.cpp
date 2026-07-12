#include "HttpClientActivity.h"

#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---- helpers ----

int HttpClientActivity::getMenuItemCount() const {
  // GET: Method, URL, Send  (3)
  // POST: Method, URL, Body, Send  (4)
  return (method == POST) ? 4 : 3;
}

// ---- lifecycle ----

void HttpClientActivity::onEnter() {
  Activity::onEnter();
  state = MENU;
  url = "http://";
  menuIndex = 0;
  requestUpdate();
}

void HttpClientActivity::onExit() {
  Activity::onExit();
  responseBody.clear();
}

// ---- loop ----

void HttpClientActivity::loop() {
  if (state == MENU) {
    const int count = getMenuItemCount();

    buttonNavigator.onNext([this, count] {
      menuIndex = ButtonNavigator::nextIndex(menuIndex, count);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, count] {
      menuIndex = ButtonNavigator::previousIndex(menuIndex, count);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int sendIndex = count - 1;

      if (menuIndex == 0) {
        // Toggle method
        method = (method == GET) ? POST : GET;
        // Clamp menuIndex if we switched from POST (4 items) to GET (3 items)
        if (menuIndex >= getMenuItemCount()) menuIndex = getMenuItemCount() - 1;
        requestUpdate();
      } else if (menuIndex == 1) {
        // Enter URL
        state = ENTER_URL;
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "URL", url, 256),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                url = std::get<KeyboardResult>(result.data).text;
              }
              state = MENU;
              requestUpdate();
            });
      } else if (menuIndex == 2 && method == POST) {
        // Enter POST body
        state = ENTER_BODY;
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "POST Body", postBody, 512),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                postBody = std::get<KeyboardResult>(result.data).text;
              }
              state = MENU;
              requestUpdate();
            });
      } else if (menuIndex == sendIndex) {
        // Send request
        if (!url.empty()) {
          performRequest();
        }
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    return;
  }

  if (state == RESULT) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int visibleLines = (lineHeight > 0) ? (contentHeight / lineHeight) : 1;

    buttonNavigator.onNext([this, visibleLines] {
      const int maxOffset = (totalLines > visibleLines) ? (totalLines - visibleLines) : 0;
      scrollOffset = (scrollOffset + visibleLines < maxOffset) ? (scrollOffset + visibleLines) : maxOffset;
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, visibleLines] {
      scrollOffset = (scrollOffset - visibleLines > 0) ? (scrollOffset - visibleLines) : 0;
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      const int maxOffset = (totalLines > visibleLines) ? (totalLines - visibleLines) : 0;
      scrollOffset = (scrollOffset + visibleLines < maxOffset) ? (scrollOffset + visibleLines) : maxOffset;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      scrollOffset = (scrollOffset - visibleLines > 0) ? (scrollOffset - visibleLines) : 0;
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = MENU;
      responseBody.clear();
      scrollOffset = 0;
      totalLines = 0;
      requestUpdate();
      return;
    }
    return;
  }

  // CONNECTING / ENTER_URL / ENTER_BODY: no input handling needed here
}

// ---- performRequest ----

void HttpClientActivity::performRequest() {
  if (WiFi.status() != WL_CONNECTED) {
    RADIO.ensureWifi();
    startActivityForResult(
        std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
        [this](const ActivityResult& result) {
          if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
            state = MENU;
            requestUpdate();
          } else {
            performRequest();  // retry after connected
          }
        });
    return;
  }

  state = CONNECTING;
  requestUpdate(true);  // immediate update to show "Connecting..."

  HTTPClient http;
  http.begin(url.c_str());
  http.setTimeout(5000);

  unsigned long start = millis();
  responseCode = (method == GET) ? http.GET() : http.POST(postBody.c_str());
  responseTimeMs = millis() - start;

  if (responseCode > 0) {
    String body = http.getString();
    if (body.length() > static_cast<unsigned int>(MAX_RESPONSE_BYTES)) {
      responseBody = std::string(body.c_str(), MAX_RESPONSE_BYTES);
    } else {
      responseBody = body.c_str();
    }
  } else {
    responseBody = "Connection failed (error " + std::to_string(responseCode) + ")";
  }
  http.end();

  state = RESULT;
  scrollOffset = 0;
  totalLines = 0;
  requestUpdate();
}

// ---- render ----

void HttpClientActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == MENU) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "HTTP Client");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int count = getMenuItemCount();

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, menuIndex,
        [this](int i) -> std::string {
          const int sendIndex = getMenuItemCount() - 1;
          if (i == 0) {
            return (method == GET) ? "Method: GET" : "Method: POST";
          } else if (i == 1) {
            if (url.empty() || url == "http://") return "URL: (not set)";
            // Truncate long URLs for display
            const int maxW = renderer.getScreenWidth() - 2 * UITheme::getInstance().getMetrics().contentSidePadding;
            std::string label = "URL: " + url;
            return renderer.truncatedText(UI_10_FONT_ID, label.c_str(), maxW);
          } else if (i == 2 && method == POST) {
            return postBody.empty() ? "Body: (empty)" : ("Body: " + postBody);
          } else if (i == sendIndex) {
            return "> Send Request";
          }
          return "";
        },
        nullptr, nullptr);

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == CONNECTING) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "HTTP Client");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Connecting...");

  } else if (state == RESULT) {
    // Build subtitle: "200 — 342ms"
    char subtitleBuf[48];
    snprintf(subtitleBuf, sizeof(subtitleBuf), "%d \xe2\x80\x94 %lums", responseCode, responseTimeMs);

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "HTTP Client",
                   subtitleBuf);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textWidth = pageWidth - 2 * metrics.contentSidePadding;

    // Word-wrap response body; 0 = no line limit
    auto lines = renderer.wrappedText(UI_10_FONT_ID, responseBody.c_str(), textWidth, 0);
    totalLines = static_cast<int>(lines.size());

    const int visibleLines = (lineHeight > 0) ? (contentHeight / lineHeight) : 1;

    // Draw visible lines
    const int endLine = scrollOffset + visibleLines;
    int y = contentTop;
    for (int i = scrollOffset; i < endLine && i < totalLines; i++) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, lines[i].c_str());
      y += lineHeight;
    }

    // Page indicator at bottom-right
    if (totalLines > visibleLines) {
      const int currentPage = (visibleLines > 0) ? (scrollOffset / visibleLines + 1) : 1;
      const int totalPages = (visibleLines > 0) ? ((totalLines + visibleLines - 1) / visibleLines) : 1;
      char pageBuf[24];
      snprintf(pageBuf, sizeof(pageBuf), "%d/%d", currentPage, totalPages);
      const int pageIndW = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
      const int pageIndY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                           renderer.getLineHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, pageWidth - metrics.contentSidePadding - pageIndW, pageIndY, pageBuf);
    }

    const auto labels = mappedInput.mapLabels("Back", "", "Pg Up", "Pg Dn");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
