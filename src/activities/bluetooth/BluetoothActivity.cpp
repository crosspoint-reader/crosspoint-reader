#include "BluetoothActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

#define DEVICE_NAME         "EPaper"
#define SERVICE_UUID        "4ae29d01-499a-480a-8c41-a82192105125"
#define REQUEST_CHARACTERISTIC_UUID  "a00e530d-b48b-48c8-aadb-d062a1b91792"
#define RESPONSE_CHARACTERISTIC_UUID "0c656023-dee6-47c5-9afb-e601dfbdaa1d"

#define OUTPUT_DIRECTORY "/bt"

#define PROTOCOL_ASSERT(cond, fmt, ...)                            \
    do {                                                           \
        if (!(cond))                                               \
        {                                                          \
            snprintf(errorMessage, sizeof(errorMessage), fmt, ##__VA_ARGS__); \
            intoState(STATE_ERROR);                                \
            return;                                                \
        }                                                          \
    } while (0)

void BluetoothActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BluetoothActivity*>(param);
  self->displayTaskLoop();
}

void BluetoothActivity::startAdvertising() {
  NimBLEDevice::startAdvertising();
}

void BluetoothActivity::stopAdvertising() {
  NimBLEDevice::stopAdvertising();
}

void BluetoothActivity::onEnter() {
  Activity::onEnter();

  NimBLEDevice::init(DEVICE_NAME);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks, false);
  pService = pServer->createService(SERVICE_UUID);
  pRequestChar = pService->createCharacteristic(
    REQUEST_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRequestChar->setCallbacks(&requestCallbacks);
  pResponseChar = pService->createCharacteristic(
    RESPONSE_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::INDICATE
  );
  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(DEVICE_NAME);
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->enableScanResponse(true);

  renderingMutex = xSemaphoreCreateMutex();

  state = STATE_INITIALIZING;
  intoState(STATE_WAITING);

  xTaskCreate(&BluetoothActivity::taskTrampoline, "BluetoothTask",
              // TODO: figure out how much stack we actually need
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void BluetoothActivity::intoState(State newState) {
  if (state == newState) {
    return;
  }

  switch (newState) {
    case STATE_WAITING:
      file.close();
      startAdvertising();
      txnId = 0;
      break;
    case STATE_OFFERED:
      // caller sets filename, totalBytes, file, txnId
      receivedBytes = 0;
      break;
    case STATE_ERROR:
    {
      // caller sets errorMessage
      file.close();
      if (pServer->getConnectedCount() > 0) {
        // TODO: send back a response over BLE?
        pServer->disconnect(pServer->getPeerInfo(0));
      }
      break;
    }
  }

  state = newState;
  updateRequired = true;
}

void BluetoothActivity::onExit() {
  Activity::onExit();

  file.close();

  stopAdvertising();

  NimBLEDevice::deinit(true);

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void BluetoothActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  if (state == STATE_ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // restart
      intoState(STATE_WAITING);
    }
  }
}

void BluetoothActivity::displayTaskLoop() {
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

void BluetoothActivity::render() const {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Bluetooth", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 50, "Use the Longform app to transfer files.");

  std::string stateText;
  switch (state) {
    case STATE_WAITING:
      stateText = "Waiting for a connection.";
      break;
    case STATE_CONNECTED:
      stateText = "Connected.";
      break;
    case STATE_OFFERED:
      stateText = "Ready to receive.";
      break;
    case STATE_RECEIVING:
      stateText = "Receiving.";
      break;
    case STATE_DONE:
      stateText = "Transfer complete.";
      break;
    case STATE_ERROR:
      stateText = "An error occurred.";
      break;
    default:
      stateText = "UNKNOWN STATE.";
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 75, stateText.c_str());

  if (state == STATE_OFFERED || state == STATE_RECEIVING || state == STATE_DONE) {
    renderer.drawCenteredText(UI_12_FONT_ID, 110, filename);
  } else if (state == STATE_ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, 110, errorMessage);
  }

  if (state == STATE_RECEIVING) {
    const int percent = (totalBytes > 0) ? (receivedBytes * 100) / totalBytes : 0;

    const int barWidth = renderer.getScreenWidth() * 3 / 4;
    const int barHeight = 20;
    const int boxX = (renderer.getScreenWidth() - barWidth) / 2;
    const int boxY = 160;
    renderer.drawRect(boxX, boxY, barWidth, barHeight);
    const int fillWidth = (barWidth - 2) * percent / 100;
    renderer.fillRect(boxX + 1, boxY + 1, fillWidth, barHeight - 2);

    char dynamicText[64];
    snprintf(dynamicText, sizeof(dynamicText), "Received %zu / %zu bytes (%d%%)", receivedBytes, totalBytes, percent);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, dynamicText);
  }

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(
    "« Back",
    (state == STATE_ERROR) ? "Restart" : "",
    "",
    ""
  );
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothActivity::ServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  Serial.printf("BLE connected\n");
  activity->onConnected(true);
}

void BluetoothActivity::ServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
  Serial.printf("BLE disconnected\n");
  activity->onConnected(false);
}

void BluetoothActivity::onConnected(bool isConnected) {
  if (state == STATE_ERROR) {
    // stay in error state so the user can read the error message even after disconnect
    return;
  }

  intoState(isConnected ? STATE_CONNECTED : STATE_WAITING);
}

void BluetoothActivity::onRequest(lfbt_message* msg, size_t msg_len) {
  if (state == STATE_ERROR) {
    // ignore further messages in error state
    return;
  }

  PROTOCOL_ASSERT((txnId == 0) || (txnId == msg->txnId), "Multiple transfers happening at once (%x != %x)", txnId, msg->txnId);

  switch (msg->type) {
    case 0: // client_offer
    {
      PROTOCOL_ASSERT(state == STATE_CONNECTED, "Invalid state for client_offer: %d", state);
      PROTOCOL_ASSERT(msg->body.clientOffer.version == 1, "Unsupported protocol version: %u", msg->body.clientOffer.version);

      totalBytes = msg->body.clientOffer.bodyLength;
      
      size_t filenameLen = msg_len - 8 - sizeof(lfbt_msg_client_offer);
      if (filenameLen > MAX_FILENAME) {
        filenameLen = MAX_FILENAME;
      }

      memcpy(filename, msg->body.clientOffer.name, filenameLen);
      filename[filenameLen] = 0;

      // sanitize filename
      for (char *p = filename; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
          *p = '_';
        }
      }

      PROTOCOL_ASSERT(SdMan.ensureDirectoryExists(OUTPUT_DIRECTORY), "Couldn't create output directory %s", OUTPUT_DIRECTORY);
      char filepath[MAX_FILENAME + strlen(OUTPUT_DIRECTORY) + 2];
      snprintf(filepath, sizeof(filepath), "%s/%s", OUTPUT_DIRECTORY, filename);
      // TODO: we could check if file already exists and append a number to filename to avoid overwriting
      PROTOCOL_ASSERT(SdMan.openFileForWrite("BT", filepath, file), "Couldn't open file %s for writing", filepath);
      // TODO: would be neat to check if we have enough space, but SDCardManager doesn't seem to expose that info currently

      txnId = msg->txnId;

      intoState(STATE_OFFERED);

      lfbt_message response = {
        .type = 1, // server_response
        .txnId = txnId,
        .body = {.serverResponse = {.status = 0}}
      };
      pResponseChar->setValue((uint8_t*)&response, 8 + sizeof(lfbt_msg_server_response));
      pResponseChar->indicate(); // TODO: indicate should not be called in a callback, this ends up timing out

      updateRequired = true;
      break;
    }
    case 2: // client_chunk
    {
      Serial.printf("Received client_chunk, offset %u, length %zu\n", msg->body.clientChunk.offset, msg_len - 8 - sizeof(lfbt_msg_client_chunk));
      PROTOCOL_ASSERT(state == STATE_OFFERED || state == STATE_RECEIVING, "Invalid state for client_chunk: %d", state);
      PROTOCOL_ASSERT(msg->body.clientChunk.offset == receivedBytes, "Expected chunk %d, got %d", receivedBytes, msg->body.clientChunk.offset);

      size_t written = file.write((uint8_t*)msg->body.clientChunk.body, msg_len - 8 - sizeof(lfbt_msg_client_chunk));
      PROTOCOL_ASSERT(written > 0, "Couldn't write to file");
      receivedBytes += msg_len - 8 - sizeof(lfbt_msg_client_chunk);
      if (receivedBytes >= totalBytes) {
        PROTOCOL_ASSERT(receivedBytes == totalBytes, "Got more bytes than expected: %zu > %zu", receivedBytes, totalBytes);
        PROTOCOL_ASSERT(file.close(), "Couldn't finalize writing the file");
        // TODO: automatically open file in reader
        intoState(STATE_DONE);
      } else {
        intoState(STATE_RECEIVING);
      }
      updateRequired = true;
      break;
    }
  }
}

void BluetoothActivity::RequestCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
  lfbt_message *msg = (lfbt_message*) pCharacteristic->getValue().data();
  Serial.printf("Received BLE message of type %u, txnId %x, length %d\n", msg->type, msg->txnId, pCharacteristic->getValue().length());
  activity->onRequest(msg, pCharacteristic->getValue().length());
}
