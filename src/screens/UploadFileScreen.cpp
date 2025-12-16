#include "UploadFileScreen.h"

#include <GfxRenderer.h>
#include <SD.h>

#include "config.h"
#include "images/CrossLarge.h"
#include "server/UploadServer.h"

void UploadFileScreen::taskTrampoline(void* param) {
  auto* self = static_cast<UploadFileScreen*>(param);
  self->displayTaskLoop();
}

void UploadFileScreen::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void UploadFileScreen::render() const {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(CrossLarge, (pageWidth - 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(UI_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "UPLOADING");

  if (currentUploadStatus == InProgress) {
    const double complete = static_cast<double>(currentUploadCompleteSize) / static_cast<double>(currentUploadTotalSize);
    renderer.drawRect(20, pageHeight / 2 + 110, pageWidth - 40, 50);
    renderer.fillRect(22, pageHeight / 2 + 112, (pageWidth - 44) * complete, 46);
  }

  renderer.displayBuffer();
}

void UploadFileScreen::onFileUploadStart(AsyncWebServerRequest* request, const String& filename) {
  if (request->hasHeader("Content-Length")) {
    const String contentLengthStr = request->header("Content-Length");
    currentUploadTotalSize = contentLengthStr.toInt();
  } else {
    currentUploadTotalSize = 0;
  }
  currentUploadCompleteSize = 0;
  currentUploadFilename = filename;
  currentUploadStatus = InProgress;
  updateRequired = true;

  // First chunk of data: open the file in write mode
  // Use request->_tempFile to manage the file object across chunks
  // Writing to SD uses SPI, so lock the screen
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  request->_tempFile = SD.open("/" + filename, FILE_WRITE, true);
  xSemaphoreGive(renderingMutex);
}

void UploadFileScreen::onFileUploadPart(AsyncWebServerRequest* request, const uint8_t* data, const size_t len) {
  // Write the received chunk of data to the file
  // Writing to SD uses SPI, so lock the screen
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  request->_tempFile.write(data, len);
  xSemaphoreGive(renderingMutex);

  const int oldPercent = static_cast<double>(currentUploadCompleteSize) / static_cast<double>(currentUploadTotalSize) * 100;
  currentUploadCompleteSize += len;
  const int newPercent = static_cast<double>(currentUploadCompleteSize) / static_cast<double>(currentUploadTotalSize) * 100;

  // Only update the screen at most every 5% to avoid blocking the SPI channel
  if (oldPercent / 5 < newPercent / 5) {
    updateRequired = true;
  }
}

void UploadFileScreen::onFileUploadEnd(AsyncWebServerRequest* request) {
  currentUploadStatus = Complete;

  // Final chunk of data: close the file and send a response
  // Writing to SD uses SPI, so lock the screen
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  request->_tempFile.close();
  xSemaphoreGive(renderingMutex);

  updateRequired = true;
}

void UploadFileScreen::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();

  uploadServer.reset(new UploadServer(
      [this](AsyncWebServerRequest* request, const String& filename) { onFileUploadStart(request, filename); },
      [this](AsyncWebServerRequest* request, const uint8_t* data, const size_t len) {
        onFileUploadPart(request, data, len);
      },
      [this](AsyncWebServerRequest* request) { onFileUploadEnd(request); }));
  uploadServer->begin();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&UploadFileScreen::taskTrampoline, "UploadFileScreenTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void UploadFileScreen::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  uploadServer->end();
  uploadServer.reset();
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void UploadFileScreen::handleInput() {
  uploadServer->loop();

  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoHome();
  }
}
