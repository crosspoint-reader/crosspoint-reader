// BleSyncService — Build Zero BLE peripheral (GATT server) for the Xteink X4.
//
// Exposes the X4 Reading Sync Service (see PROTOCOL.md): capabilities/read,
// x4ToPhone/notify, phoneToX4/write, syncState/read. Sends dummy HAS_UPDATE /
// NEEDS_UPDATE intents and records the phone's ACK / dummy-position replies.
//
// Isolation: all BLE code lives under src/ble_sync/. Nothing here touches the
// reader, book files, or existing progress storage. Compile out entirely with
// -DENABLE_BLE_SYNC=0.
//
// RADIO NOTE (ESP32-C3): BLE and Wi-Fi contend for one 2.4 GHz radio and scarce
// heap. Bring BLE up only while Wi-Fi is down. Call stop() before starting a
// Wi-Fi workflow (KOSync/OTA/upload) and begin() again afterward.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "BleSyncProtocol.h"  // ParsedMessage for the v2 inbound queue

#ifndef ENABLE_BLE_SYNC
#define ENABLE_BLE_SYNC 1
#endif

struct BleSyncServiceImpl;  // pImpl — defined in BleSyncService.cpp

class BleSyncService {
 public:
  static BleSyncService& instance();

  // Lifecycle. deviceId is a stable id for this X4 (e.g. "x4-" + chip id).
  // Safe to call begin()/stop() repeatedly; both are no-ops when already in
  // the requested state or when ENABLE_BLE_SYNC == 0.
  void begin(const std::string& deviceId);
  void stop();
  bool isRunning() const;

  // Advertising control (advertising auto-starts on begin()).
  void startAdvertising();
  void stopAdvertising();
  bool isAdvertising() const;

  // Connection state (for the dev UI).
  bool isConnected() const;

  // Dev actions — notify the phone with a dummy sync intent.
  void sendHasUpdate();
  void sendNeedsUpdate();

  // v1: send the current/last book's real reading position to the phone.
  void sendProgress(const std::string& document, const std::string& titleHash, const std::string& xpointer,
                    float percentage, int64_t updatedAt);
  // If the phone sent a PROGRESS since the last call, fill args + return true (consumes it).
  bool takeReceivedProgress(std::string& document, std::string& titleHash, std::string& xpointer,
                            float& percentage, int64_t& updatedAt);

  // v2: send this device's book manifest (built with our deviceId + current clock).
  void sendManifest(const std::vector<BleSyncProtocol::ManifestEntry>& books, bool more);
  // v2: ask the phone to push PROGRESS for these title_hashes.
  void sendWant(const std::vector<std::string>& keys);
  // v2: send any prebuilt JSON message (MANIFEST / WANT / PROGRESS) over x4ToPhone.
  void sendMessage(const std::string& payload);
  // v2: pop the next inbound app message (PROGRESS / MANIFEST / WANT) in FIFO
  // order. Returns false when the queue is empty. Housekeeping frames (PAIR/ACK)
  // are consumed internally and never queued.
  bool takeReceivedMessage(BleSyncProtocol::ParsedMessage& out);

  // v3: count of ACKs the phone has sent since the current connection began
  // (reset on connect). Drives PROGRESS resend gating in BleSyncManager.
  int receivedAckCount() const;

  // State for the "BLE Sync Test" dev screen.
  std::string pairedPhoneId() const;
  std::string lastPhoneResponse() const;
  std::string lastEvent() const;

  // Optional: observe events for on-screen logging. Called from the BLE task
  // context — keep handlers short and thread-aware.
  using LogHandler = std::function<void(const std::string& line)>;
  void setLogHandler(LogHandler handler);

 private:
  BleSyncService() = default;
  ~BleSyncService() = default;
  BleSyncService(const BleSyncService&) = delete;
  BleSyncService& operator=(const BleSyncService&) = delete;

  BleSyncServiceImpl* impl_ = nullptr;   // NimBLE handles hidden from callers
};
