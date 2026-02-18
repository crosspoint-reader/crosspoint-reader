#include "KOReaderPageTurnerActivity.h"

#include <GfxRenderer.h>
#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderPageTurnerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  state = WIFI_SELECTION;
  errorMessage.clear();

  // Turn on WiFi
  LOG_DBG("KPT", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KPT", "Already connected to WiFi");
    launchAddressEntry();
    return;
  }

  // Launch WiFi selection
  LOG_DBG("KPT", "Launching WifiSelectionActivity...");
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void KOReaderPageTurnerActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off WiFi
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void KOReaderPageTurnerActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();

  if (!connected) {
    LOG_DBG("KPT", "WiFi connection failed, exiting");
    onGoBack();
    return;
  }

  LOG_DBG("KPT", "WiFi connected, requesting IP address");
  launchAddressEntry();
}

void KOReaderPageTurnerActivity::launchAddressEntry() {
  state = IP_ENTRY;

  // Use cached address or default to "192.168."
  std::string initialText = "192.168.";
  if (strlen(SETTINGS.koReaderPageTurnerAddress) > 0) {
    initialText = SETTINGS.koReaderPageTurnerAddress;
  }

  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, tr(STR_KPT_ENTER_ADDRESS), initialText, 10, 63, false,
      [this](const std::string& result) { onAddressEntered(result); },
      [this]() {
        exitActivity();
        onGoBack();
      }));
}

void KOReaderPageTurnerActivity::onAddressEntered(const std::string& address) {
  exitActivity();

  deviceAddress = address;

  // Cache the address in settings
  strncpy(SETTINGS.koReaderPageTurnerAddress, address.c_str(), sizeof(SETTINGS.koReaderPageTurnerAddress) - 1);
  SETTINGS.koReaderPageTurnerAddress[sizeof(SETTINGS.koReaderPageTurnerAddress) - 1] = '\0';
  SETTINGS.saveToFile();

  LOG_DBG("KPT", "Address set: %s", deviceAddress.c_str());

  state = ACTIVE;

  // Force an immediate render since we're transitioning from a subactivity
  {
    RenderLock lock(*this);
    render(std::move(lock));
  }
}

bool KOReaderPageTurnerActivity::sendPageTurn(const int direction) {
  // Build URL: if address contains ':', use as-is; otherwise append default port
  std::string host = deviceAddress;
  if (host.find(':') == std::string::npos) {
    host += ":8080";
  }

  std::string url = "http://" + host + "/koreader/event/GotoViewRel/" + std::to_string(direction);

  LOG_DBG("KPT", "Sending: %s", url.c_str());

  WiFiClient client;
  HTTPClient http;

  http.begin(client, url.c_str());
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const int httpCode = http.GET();
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    LOG_DBG("KPT", "Page turn success: %d", httpCode);
    return true;
  }

  LOG_ERR("KPT", "Page turn failed: %d", httpCode);
  return false;
}

void KOReaderPageTurnerActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == ACTIVE) {
    // Back button exits
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }

    // Confirm button allows changing the IP address
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      launchAddressEntry();
      return;
    }

    // Page forward
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (!sendPageTurn(1)) {
        errorMessage = std::string(tr(STR_KPT_ERROR_PREFIX)) + "Failed to turn page forward";
        requestUpdate();
      } else if (!errorMessage.empty()) {
        errorMessage.clear();
        requestUpdate();
      }
      return;
    }

    // Page back
    if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (!sendPageTurn(-1)) {
        errorMessage = std::string(tr(STR_KPT_ERROR_PREFIX)) + "Failed to turn page back";
        requestUpdate();
      } else if (!errorMessage.empty()) {
        errorMessage.clear();
        requestUpdate();
      }
      return;
    }
  }
}

void KOReaderPageTurnerActivity::render(Activity::RenderLock&&) {
  if (subActivity) {
    return;
  }

  if (state != ACTIVE) {
    return;
  }

  constexpr int LINE_SPACING = 28;

  renderer.clearScreen();

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_KOREADER_PAGE_TURNER), true, EpdFontFamily::BOLD);

  // Instructions
  int y = 70;
  renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_1));
  y += LINE_SPACING;
  renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_2));
  y += LINE_SPACING;
  renderer.drawText(SMALL_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_3));
  y += LINE_SPACING;
  renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_4));
  y += LINE_SPACING;
  renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_5));
  y += LINE_SPACING;
  renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_KPT_INSTRUCTION_6));

  // Connected address
  y += LINE_SPACING * 2;
  std::string targetInfo = std::string(tr(STR_KPT_CONNECTED_TO)) + deviceAddress;
  renderer.drawCenteredText(UI_10_FONT_ID, y, targetInfo.c_str(), true, EpdFontFamily::BOLD);

  // Hint
  y += LINE_SPACING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_KPT_PAGE_TURN_HINT));

  // Error message (if any)
  if (!errorMessage.empty()) {
    y += LINE_SPACING * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, errorMessage.c_str(), true, EpdFontFamily::BOLD);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_KPT_CHANGE_IP), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
