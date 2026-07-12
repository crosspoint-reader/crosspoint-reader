#include "WifiCredsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

static constexpr const char* AUTH_LABELS[] = {"WPA/WPA2", "WEP", "Open (no password)"};
static constexpr int AUTH_COUNT = 3;

// ---- keyboard launch helpers ----

void WifiCredsActivity::launchSsidKeyboard() {
  state = INPUT_SSID;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "WiFi SSID", "", 32),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
        } else {
          ssid = std::get<KeyboardResult>(result.data).text;
          launchPassKeyboard();
        }
      });
}

void WifiCredsActivity::launchPassKeyboard() {
  state = INPUT_PASS;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "WiFi Password", "", 63),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
        } else {
          password = std::get<KeyboardResult>(result.data).text;
          authType = 0;
          state = SELECT_AUTH;
          requestUpdate();
        }
      });
}

// ---- Activity lifecycle ----

void WifiCredsActivity::onEnter() {
  Activity::onEnter();
  ssid.clear();
  password.clear();
  authType = 0;
  launchSsidKeyboard();
}

void WifiCredsActivity::loop() {
  if (state == SELECT_AUTH) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      authType = (authType - 1 + AUTH_COUNT) % AUTH_COUNT;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      authType = (authType + 1) % AUTH_COUNT;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = DISPLAY_QR;
      requestUpdate();
    }
    return;
  }

  if (state == DISPLAY_QR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Start over from SSID entry
      ssid.clear();
      password.clear();
      authType = 0;
      launchSsidKeyboard();
    }
  }
}

// ---- rendering ----

void WifiCredsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "WiFi QR Share");

  if (state == SELECT_AUTH) {
    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    int y = headerBottom + metrics.verticalSpacing * 2;

    // SSID label (truncated to fit)
    char ssidBuf[48];
    if (ssid.size() > 28) {
      snprintf(ssidBuf, sizeof(ssidBuf), "SSID: %.25s...", ssid.c_str());
    } else {
      snprintf(ssidBuf, sizeof(ssidBuf), "SSID: %s", ssid.c_str());
    }
    renderer.drawCenteredText(UI_10_FONT_ID, y, ssidBuf);
    y += 30;

    renderer.drawCenteredText(SMALL_FONT_ID, y, "Select auth type:");
    y += metrics.verticalSpacing * 2;

    // Auth type list — draw each option, highlight the selected one
    for (int i = 0; i < AUTH_COUNT; i++) {
      if (i == authType) {
        // Highlight row
        const int rowH = metrics.headerHeight;
        const int rowY = y - 4;
        renderer.fillRect(20, rowY, pageWidth - 40, rowH, true);
        renderer.drawCenteredText(UI_10_FONT_ID, y, AUTH_LABELS[i], false);
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, y, AUTH_LABELS[i]);
      }
      y += metrics.headerHeight + 4;
    }

    renderer.drawCenteredText(SMALL_FONT_ID, y + 8, "Up/Down: change  Confirm: generate QR");

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == DISPLAY_QR) {
    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    const int hintH = metrics.buttonHintsHeight;
    const int ssidLabelH = 28;
    const int startY = headerBottom + metrics.verticalSpacing;
    const int availableHeight =
        pageHeight - startY - hintH - metrics.verticalSpacing - ssidLabelH;
    const int availableWidth = pageWidth - 40;

    const Rect qrBounds(20, startY, availableWidth, availableHeight);
    const std::string uri = buildWifiUri();
    QrUtils::drawQrCode(renderer, qrBounds, uri);

    // SSID label below the QR area
    char ssidBuf[48];
    if (ssid.size() > 28) {
      snprintf(ssidBuf, sizeof(ssidBuf), "%.25s...", ssid.c_str());
    } else {
      snprintf(ssidBuf, sizeof(ssidBuf), "%s", ssid.c_str());
    }
    const int labelY = startY + availableHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, labelY, ssidBuf, true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "New", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

// ---- URI builder ----

std::string WifiCredsActivity::buildWifiUri() const {
  std::string uri = "WIFI:T:";
  switch (authType) {
    case 0: uri += "WPA";    break;
    case 1: uri += "WEP";    break;
    case 2: uri += "nopass"; break;
  }
  uri += ";S:";
  for (char c : ssid) {
    if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') uri += '\\';
    uri += c;
  }
  uri += ";P:";
  if (authType != 2) {
    for (char c : password) {
      if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') uri += '\\';
      uri += c;
    }
  }
  uri += ";;";
  return uri;
}
