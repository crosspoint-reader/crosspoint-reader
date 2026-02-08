#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KOReaderAuthActivity*>(param);
  self->displayTaskLoop();
}

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    errorMessage = "WiFi connection failed";
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = AUTHENTICATING;
  statusMessage = "Authenticating...";
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

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
  updateRequired = true;
}

void KOReaderAuthActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  xTaskCreate(&KOReaderAuthActivity::taskTrampoline, "KOAuthTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Turn on WiFi
  WiFi.mode(WIFI_STA);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    state = AUTHENTICATING;
    statusMessage = "Authenticating...";
    updateRequired = true;

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

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void KOReaderAuthActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KOReaderAuthActivity::render() {
  if (subActivity) {
    return;
  }

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "KOReader Sync");
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Success!", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, "KOReader sync is ready to use");
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Error", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
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
