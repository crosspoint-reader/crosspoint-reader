#include "TrmnlRegistrationActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "trmnl/TrmnlService.h"

TrmnlRegistrationActivity::TrmnlRegistrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     OnFinishCallback onFinish)
    : ActivityWithSubactivity("TrmnlRegistration", renderer, mappedInput), onFinish(std::move(onFinish)) {}

void TrmnlRegistrationActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  LOG_DBG("TRMNL", "Turning on WiFi for registration...");
  WiFi.mode(WIFI_STA);

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
  } else {
    LOG_DBG("TRMNL", "Launching WifiSelectionActivity...");
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  }
}

void TrmnlRegistrationActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // WiFi is left on, as the user might want to test immediately after.
  // It will be turned off when exiting the main settings activity.
}

void TrmnlRegistrationActivity::onWifiSelectionComplete(bool success) {
  exitActivity();

  if (!success) {
    LOG_ERR("TRMNL", "WiFi connection failed");
    state = State::FAILURE;
    statusMessage = "WiFi Connection Failed";
    requestUpdate();
    return;
  }

  LOG_DBG("TRMNL", "WiFi connected, starting registration");
  state = State::REGISTERING;
  statusMessage = "Registering...";
  requestUpdate();
  requestUpdateAndWait(); // Force a render before blocking call

  // Perform registration
  if (TrmnlService::registerDevice()) {
    LOG_DBG("TRMNL", "Registration successful");
    state = State::SUCCESS;
    statusMessage = "Registration Successful";
  } else {
    LOG_ERR("TRMNL", "Registration failed");
    state = State::FAILURE;
    statusMessage = "Registration Failed";
  }
  requestUpdate();
}

void TrmnlRegistrationActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onFinish();
  }
}

void TrmnlRegistrationActivity::render(RenderLock&&) {
  if (subActivity) {
    return;
  }

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Register Device", true, EpdFontFamily::BOLD);

  std::string message;
  switch (state) {
    case State::REGISTERING:
      message = statusMessage;
      break;
    case State::SUCCESS:
      message = statusMessage;
      break;
    case State::FAILURE:
      message = statusMessage;
      break;
    case State::WIFI_SELECTION:
      // WifiSelectionActivity is rendering
      return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, 300, message.c_str(), true, EpdFontFamily::BOLD);

  if (state == State::SUCCESS || state == State::FAILURE) {
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}