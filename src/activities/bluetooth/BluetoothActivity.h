#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#include <functional>

#include <SDCardManager.h>

#include "../Activity.h"

#define MAX_FILENAME 200

typedef struct  __attribute__((packed)) {
  uint32_t version;
  uint32_t bodyLength;
  uint32_t nameLength;
  char name[];
} lfbt_msg_client_offer; // msg type 0

typedef struct  __attribute__((packed)) {
  uint32_t status;
} lfbt_msg_server_response; // msg type 1

typedef struct  __attribute__((packed)) {
  uint32_t offset;
  char body[];
} lfbt_msg_client_chunk; // msg type 2

typedef union {
  lfbt_msg_client_offer clientOffer;
  lfbt_msg_server_response serverResponse;
  lfbt_msg_client_chunk clientChunk;
} lfbt_message_body;

typedef struct __attribute__((packed)) {
  uint32_t type;
  uint32_t txnId;
  lfbt_message_body body;
} lfbt_message;

/**
 * BluetoothActivity receives files over a custom BLE protocol and stores them on the SD card.
 * 
 * The onCancel callback is called if the user presses back.
 */
class BluetoothActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void onConnected(bool isConnected);
  void onRequest(lfbt_message *msg, size_t msg_len);

  class ServerCallbacks : public BLEServerCallbacks {
    friend class BluetoothActivity;
    BluetoothActivity *activity;

    void onConnect(BLEServer* pServer);
    void onDisconnect(BLEServer* pServer);

    protected:
    explicit ServerCallbacks(BluetoothActivity *activity) : activity(activity) {}
  };

  ServerCallbacks serverCallbacks;

  class RequestCallbacks : public BLECharacteristicCallbacks {
    friend class BluetoothActivity;
    BluetoothActivity *activity;

    void onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param);

    protected:
    explicit RequestCallbacks(BluetoothActivity *activity) : activity(activity) {}
  };

  RequestCallbacks requestCallbacks;

  BLEServer *pServer;
  BLEService *pService;
  BLECharacteristic *pRequestChar, *pResponseChar;
  BLEAdvertising *pAdvertising;
  void startAdvertising();
  void stopAdvertising();

  typedef enum {
    STATE_INITIALIZING,
    STATE_WAITING,
    STATE_CONNECTED,
    STATE_OFFERED,
    STATE_RECEIVING,
    STATE_DONE,
    STATE_ERROR
  } State;

  State state = STATE_INITIALIZING;
  char filename[MAX_FILENAME + 1];
  FsFile file;
  size_t receivedBytes = 0;
  size_t totalBytes = 0; 
  char errorMessage[256];
  uint32_t txnId;

  void intoState(State newState);

 public:
  explicit BluetoothActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& onCancel)
      : Activity("Bluetooth", renderer, mappedInput), onCancel(onCancel),
        serverCallbacks(this), requestCallbacks(this) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
