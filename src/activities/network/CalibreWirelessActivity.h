#pragma once
#include <SDCardManager.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "activities/Activity.h"

/**
 * CalibreWirelessActivity implements Calibre's "wireless device" protocol.
 * This allows Calibre desktop to send books directly to the device over WiFi.
 *
 * Protocol specification sourced from Calibre's smart device driver:
 * https://github.com/kovidgoyal/calibre/blob/master/src/calibre/devices/smart_device_app/driver.py
 */
class CalibreWirelessActivity final : public Activity {
  enum class WirelessState {
    DISCOVERING,
    CONNECTING,
    WAITING,
    RECEIVING,
    COMPLETE,
    DISCONNECTED,
    ERROR
  };

  enum OpCode : uint8_t {
    OK = 0,
    SET_CALIBRE_DEVICE_INFO = 1,
    SET_CALIBRE_DEVICE_NAME = 2,
    GET_DEVICE_INFORMATION = 3,
    TOTAL_SPACE = 4,
    FREE_SPACE = 5,
    GET_BOOK_COUNT = 6,
    SEND_BOOKLISTS = 7,
    SEND_BOOK = 8,
    GET_INITIALIZATION_INFO = 9,
    BOOK_DONE = 11,
    NOOP = 12,
    DELETE_BOOK = 13,
    GET_BOOK_FILE_SEGMENT = 14,
    GET_BOOK_METADATA = 15,
    SEND_BOOK_METADATA = 16,
    DISPLAY_MESSAGE = 17,
    CALIBRE_BUSY = 18,
    SET_LIBRARY_INFO = 19,
    ERROR = 20,
  };

  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t networkTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  SemaphoreHandle_t stateMutex = nullptr;
  bool updateRequired = false;
  volatile bool shouldExit = false;

  WirelessState state = WirelessState::DISCOVERING;
  const std::function<void()> onCompleteCallback;

  WiFiUDP udp;
  WiFiClient tcpClient;
  std::string calibreHost;
  uint16_t calibrePort = 0;
  uint16_t calibreAltPort = 0;
  std::string calibreHostname;

  std::string currentFilename;
  size_t currentFileSize = 0;
  size_t bytesReceived = 0;
  std::string statusMessage;
  std::string errorMessage;

  bool inBinaryMode = false;
  size_t binaryBytesRemaining = 0;
  FsFile currentFile;
  std::string recvBuffer;

  bool inSkipMode = false;
  size_t skipBytesRemaining = 0;
  int skipOpcode = -1;
  std::string skipExtractedLpath;
  size_t skipExtractedLength = 0;

  static void displayTaskTrampoline(void* param);
  static void networkTaskTrampoline(void* param);
  void displayTaskLoop();
  void networkTaskLoop();
  void render() const;

  void listenForDiscovery();
  void handleTcpClient();
  bool readJsonMessage(std::string& message);
  void sendJsonResponse(OpCode opcode, const std::string& data);
  void handleCommand(OpCode opcode, const std::string& data);
  void receiveBinaryData();

  void handleGetInitializationInfo(const std::string& data);
  void handleGetDeviceInformation();
  void handleFreeSpace();
  void handleGetBookCount();
  void handleSendBook(const std::string& data);
  void handleSendBookMetadata(const std::string& data);
  void handleDisplayMessage(const std::string& data);
  void handleNoop(const std::string& data);

  std::string getDeviceUuid() const;
  void setState(WirelessState newState);
  void setStatus(const std::string& message);
  void setError(const std::string& message);

 public:
  explicit CalibreWirelessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onComplete)
      : Activity("CalibreWireless", renderer, mappedInput), onCompleteCallback(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }
};
