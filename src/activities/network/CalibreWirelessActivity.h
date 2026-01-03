#pragma once
#include <SDCardManager.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <string>

#include "activities/Activity.h"

// Calibre wireless device states
enum class CalibreWirelessState {
  DISCOVERING,   // Listening for Calibre server broadcasts
  CONNECTING,    // Establishing TCP connection
  WAITING,       // Connected, waiting for commands
  RECEIVING,     // Receiving a book file
  COMPLETE,      // Transfer complete
  DISCONNECTED,  // Calibre disconnected
  ERROR          // Connection/transfer error
};

/**
 * CalibreWirelessActivity implements Calibre's "wireless device" protocol.
 * This allows Calibre desktop to send books directly to the device over WiFi.
 *
 * Protocol:
 * 1. Device listens on UDP ports 54982, 48123, 39001, 44044, 59678
 * 2. Calibre broadcasts discovery messages
 * 3. Device responds with its TCP server address
 * 4. Calibre connects via TCP and sends JSON commands
 * 5. Books are transferred as binary data after SEND_BOOK command
 */
class CalibreWirelessActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t networkTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  SemaphoreHandle_t stateMutex = nullptr;
  bool updateRequired = false;

  CalibreWirelessState state = CalibreWirelessState::DISCOVERING;
  const std::function<void()> onComplete;

  // UDP discovery
  WiFiUDP udp;
  static constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
  static constexpr size_t UDP_PORT_COUNT = 5;
  static constexpr uint16_t LOCAL_UDP_PORT = 8134;  // Port to receive responses

  // TCP connection (we connect to Calibre)
  WiFiClient tcpClient;
  std::string calibreHost;
  uint16_t calibrePort = 0;
  uint16_t calibreAltPort = 0;  // Alternative port (content server)
  std::string calibreHostname;

  // Transfer state
  std::string currentFilename;
  size_t currentFileSize = 0;
  size_t bytesReceived = 0;
  std::string statusMessage;
  std::string errorMessage;

  // Protocol state
  bool inBinaryMode = false;
  size_t binaryBytesRemaining = 0;
  FsFile currentFile;

  // Calibre protocol opcodes (from calibre/devices/smart_device_app/driver.py)
  static constexpr int OP_OK = 0;
  static constexpr int OP_SET_CALIBRE_DEVICE_INFO = 1;
  static constexpr int OP_SET_CALIBRE_DEVICE_NAME = 2;
  static constexpr int OP_GET_DEVICE_INFORMATION = 3;
  static constexpr int OP_TOTAL_SPACE = 4;
  static constexpr int OP_FREE_SPACE = 5;
  static constexpr int OP_GET_BOOK_COUNT = 6;
  static constexpr int OP_SEND_BOOKLISTS = 7;
  static constexpr int OP_SEND_BOOK = 8;
  static constexpr int OP_GET_INITIALIZATION_INFO = 9;
  static constexpr int OP_BOOK_DONE = 11;
  static constexpr int OP_NOOP = 12;  // Was incorrectly 18
  static constexpr int OP_DELETE_BOOK = 13;
  static constexpr int OP_GET_BOOK_FILE_SEGMENT = 14;
  static constexpr int OP_GET_BOOK_METADATA = 15;
  static constexpr int OP_SEND_BOOK_METADATA = 16;
  static constexpr int OP_DISPLAY_MESSAGE = 17;
  static constexpr int OP_CALIBRE_BUSY = 18;
  static constexpr int OP_SET_LIBRARY_INFO = 19;
  static constexpr int OP_ERROR = 20;

  static void displayTaskTrampoline(void* param);
  static void networkTaskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  [[noreturn]] void networkTaskLoop();
  void render() const;

  // Network operations
  void listenForDiscovery();
  void handleTcpClient();
  bool readJsonMessage(std::string& message);
  void sendJsonResponse(int opcode, const std::string& data);
  void handleCommand(int opcode, const std::string& data);
  void receiveBinaryData();

  // Protocol handlers
  void handleGetInitializationInfo(const std::string& data);
  void handleGetDeviceInformation();
  void handleFreeSpace();
  void handleGetBookCount();
  void handleSendBook(const std::string& data);
  void handleSendBookMetadata(const std::string& data);
  void handleDisplayMessage(const std::string& data);
  void handleNoop(const std::string& data);

  // Utility
  std::string sanitizeFilename(const std::string& title) const;
  std::string getDeviceUuid() const;
  void setState(CalibreWirelessState newState);
  void setStatus(const std::string& message);
  void setError(const std::string& message);

 public:
  explicit CalibreWirelessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onComplete)
      : Activity("CalibreWireless", renderer, mappedInput), onComplete(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }
};
