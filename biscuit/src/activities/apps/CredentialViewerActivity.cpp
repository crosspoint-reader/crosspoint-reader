#include "CredentialViewerActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void CredentialViewerActivity::onEnter() {
  Activity::onEnter();
  loadCredentials();
  requestUpdate();
}

void CredentialViewerActivity::onExit() {
  Activity::onExit();
}

void CredentialViewerActivity::loadCredentials() {
  creds.clear();

  String content = Storage.readFile("/biscuit/creds.csv");
  if (content.length() == 0) {
    state = EMPTY_VIEW;
    return;
  }

  // Split into lines
  int lineStart = 0;
  bool firstLine = true;
  const int len = static_cast<int>(content.length());

  for (int i = 0; i <= len; i++) {
    if (i == len || content[i] == '\n') {
      // Extract line (strip trailing \r if present)
      int lineEnd = i;
      if (lineEnd > lineStart && content[lineEnd - 1] == '\r') lineEnd--;

      if (lineEnd > lineStart) {
        if (firstLine) {
          // Skip header row
          firstLine = false;
        } else {
          // Parse CSV: timestamp,ssid,username,password
          Credential cred;
          int fieldStart = lineStart;
          int field = 0;

          for (int j = lineStart; j <= lineEnd; j++) {
            if (j == lineEnd || content[j] == ',') {
              std::string val(content.c_str() + fieldStart, j - fieldStart);
              switch (field) {
                case 0: cred.timestamp = val; break;
                case 1: cred.ssid = val; break;
                case 2: cred.username = val; break;
                case 3: cred.password = val; break;
                default: break;
              }
              field++;
              fieldStart = j + 1;
            }
          }

          if (field >= 2) {
            creds.push_back(std::move(cred));
          }
        }
      }
      lineStart = i + 1;
    }
  }

  if (creds.empty()) {
    state = EMPTY_VIEW;
  } else {
    state = LIST_VIEW;
    selectorIndex = 0;
  }
}

void CredentialViewerActivity::deleteAllCredentials() {
  Storage.remove("/biscuit/creds.csv");
  creds.clear();
  state = EMPTY_VIEW;
}

void CredentialViewerActivity::loop() {
  switch (state) {
    case EMPTY_VIEW: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;
    }

    case LIST_VIEW: {
      const int count = static_cast<int>(creds.size());

      buttonNavigator.onNext([this, count] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
        requestUpdate();
      });

      buttonNavigator.onPrevious([this, count] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
        requestUpdate();
      });

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (mappedInput.getHeldTime() >= 500) {
          state = CONFIRM_DELETE;
        } else if (!creds.empty()) {
          state = DETAIL_VIEW;
        }
        requestUpdate();
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;
    }

    case DETAIL_VIEW: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = LIST_VIEW;
        requestUpdate();
      }
      break;
    }

    case CONFIRM_DELETE: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        deleteAllCredentials();
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = LIST_VIEW;
        requestUpdate();
      }
      break;
    }
  }
}

void CredentialViewerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  switch (state) {
    case EMPTY_VIEW: {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Credentials");
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "No captured credentials");
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, "Use portal to capture");
      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case LIST_VIEW: {
      std::string subtitle = std::to_string(creds.size()) + " entries";
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Credentials",
                     subtitle.c_str());

      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(creds.size()), selectorIndex,
          [this](int i) -> std::string {
            return creds[i].username.empty() ? "(no username)" : creds[i].username;
          },
          [this](int i) -> std::string {
            return creds[i].ssid + " - " + creds[i].timestamp;
          });

      const auto labels = mappedInput.mapLabels("Exit", "Hold:Delete", "^", "v");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case DETAIL_VIEW: {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Credential Detail");

      if (selectorIndex < static_cast<int>(creds.size())) {
        const auto& cred = creds[selectorIndex];
        const int leftPad = metrics.contentSidePadding;
        int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
        const int lineH = 45;

        renderer.drawText(SMALL_FONT_ID, leftPad, y, "Timestamp", true, EpdFontFamily::BOLD);
        y += 22;
        renderer.drawText(UI_10_FONT_ID, leftPad, y, cred.timestamp.c_str());
        y += lineH;

        renderer.drawText(SMALL_FONT_ID, leftPad, y, "SSID", true, EpdFontFamily::BOLD);
        y += 22;
        renderer.drawText(UI_10_FONT_ID, leftPad, y, cred.ssid.c_str());
        y += lineH;

        renderer.drawText(SMALL_FONT_ID, leftPad, y, "Username", true, EpdFontFamily::BOLD);
        y += 22;
        renderer.drawText(UI_10_FONT_ID, leftPad, y,
                          cred.username.empty() ? "(empty)" : cred.username.c_str());
        y += lineH;

        renderer.drawText(SMALL_FONT_ID, leftPad, y, "Password", true, EpdFontFamily::BOLD);
        y += 22;
        renderer.drawText(UI_10_FONT_ID, leftPad, y,
                          cred.password.empty() ? "(empty)" : cred.password.c_str());
      }

      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case CONFIRM_DELETE: {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Credentials");
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "Delete all credentials?", true,
                                EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  renderer.displayBuffer();
}
