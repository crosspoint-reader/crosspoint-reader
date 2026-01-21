#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEServer.h>

#include <functional>

#include <SDCardManager.h>

#include "../Activity.h"

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

  class ServerCallbacks : public NimBLEServerCallbacks {
    friend class BluetoothActivity;
    BluetoothActivity *activity;

    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo);
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason);

    protected:
    explicit ServerCallbacks(BluetoothActivity *activity) : activity(activity) {}
  };

  ServerCallbacks serverCallbacks;

  class RequestCallbacks : public NimBLECharacteristicCallbacks {
    friend class BluetoothActivity;
    BluetoothActivity *activity;

    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo);

    protected:
    explicit RequestCallbacks(BluetoothActivity *activity) : activity(activity) {}
  };

  RequestCallbacks requestCallbacks;

  NimBLEServer *pServer;
  NimBLEService *pService;
  NimBLECharacteristic *pRequestChar, *pResponseChar;
  NimBLEAdvertising *pAdvertising;
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
  std::string filename;
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
