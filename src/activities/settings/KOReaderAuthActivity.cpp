#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    errorMessage = "WiFi connection failed";
    xSemaphoreGive(renderingMutex);
    requestUpdate();
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = AUTHENTICATING;
  statusMessage = "Authenticating...";
  xSemaphoreGive(renderingMutex);
  requestUpdate();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = KOReaderSyncClient::authenticate();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (result == KOReaderSyncClient::OK) {
    state = SUCCESS;
    statusMessage = "Successfully authenticated!";
  } else {
    state = FAILED;
    errorMessage = KOReaderSyncClient::errorString(result);
  }
  xSemaphoreGive(renderingMutex);
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Turn on WiFi
  WiFi.mode(WIFI_STA);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    state = AUTHENTICATING;
    statusMessage = "Authenticating...";
    requestUpdate();

    // Perform authentication in a separate task
    xTaskCreate(
        [](void* param) {
          auto* self = static_cast<KOReaderAuthActivity*>(param);
          self->performAuthentication();
          vTaskDelete(nullptr);
        },
        "AuthTask", 4096, this, 1, nullptr);
    return;
  }

  // Launch WiFi selection
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void KOReaderAuthActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void KOReaderAuthActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "KOReader Auth", true, EpdFontFamily::BOLD);

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "Success!", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, "KOReader sync is ready to use");

    const auto labels = mappedInput.mapLabels("Done", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "Authentication Failed", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, errorMessage.c_str());

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderAuthActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onComplete();
    }
  }
}
