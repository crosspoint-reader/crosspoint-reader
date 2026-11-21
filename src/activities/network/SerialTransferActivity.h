#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"

// File transfer over the USB-C serial link (for boards whose USB port is a
// USB-to-serial bridge and cannot export the SD card as a drive). The reader
// answers a small framed binary protocol; tools/serial_bridge/crosspoint-serial.html
// (Web Serial, Chrome/Edge) turns it into the regular CrossPoint web file manager. See docs/serial-transfer.md.
class SerialTransferActivity final : public Activity {
 public:
  SerialTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SerialTransfer", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  static constexpr size_t MAX_PAYLOAD = 4096;
  static constexpr unsigned long FRAME_TIMEOUT_MS = 1000;
  static constexpr size_t REPLY_HEAD = 6;  // sync(2) + status + request id + length(2)

  enum class Parse : uint8_t { Sync0, Sync1, Header, Payload, Crc };

  void pump();
  void handleFrame();
  void reply(uint8_t status, const uint8_t* payload = nullptr, size_t len = 0);
  void closeFiles();
  void setAction(std::string text);

  // Commands; each returns a status code (0 = OK) and fills txBuf/txLen.
  uint8_t cmdList(const uint8_t* p, size_t n);
  uint8_t cmdReadOpen(const uint8_t* p, size_t n);
  uint8_t cmdRead(const uint8_t* p, size_t n);
  uint8_t cmdWriteOpen(const uint8_t* p, size_t n);
  uint8_t cmdWrite(const uint8_t* p, size_t n);
  uint8_t cmdWriteEnd();
  uint8_t cmdApi(const uint8_t* p, size_t n);
  uint8_t cmdApiRead(const uint8_t* p, size_t n);
  uint8_t cmdMkdir(const uint8_t* p, size_t n);
  uint8_t cmdRename(const uint8_t* p, size_t n);
  uint8_t cmdDelete(const uint8_t* p, size_t n);

  Parse parse = Parse::Sync0;
  uint8_t header[4] = {0, 0, 0, 0};
  size_t headerGot = 0;
  uint8_t cmd = 0;
  uint8_t reqId = 0;  // echoed in the reply so the host can tell replies apart
  size_t rxLen = 0;
  size_t rxGot = 0;
  uint8_t crcBytes[4] = {0, 0, 0, 0};
  size_t crcGot = 0;
  unsigned long frameStartedAt = 0;

  uint8_t* rxBuf = nullptr;  // MAX_PAYLOAD
  uint8_t* txBuf = nullptr;  // MAX_PAYLOAD + 16
  size_t txLen = 0;          // payload length produced by the current command

  HalFile readFile;
  HalFile writeFile;
  std::string writeTarget;
  std::string writePart;
  bool writeOverwrite = false;
  bool writeIsFont = false;
  bool writeIsVector = false;
  std::string apiOut;

  uint32_t pendingBaud = 0;
  unsigned long baudConfirmDeadline = 0;  // 0 = no speed change awaiting a valid frame
  bool haveEndResult = false;             // result of the last WRITE_END, kept for a retried request
  uint8_t endReqId = 0;
  uint8_t endStatus = 0;
  bool hostSeen = false;
  bool dirty = true;
  bool exitRequested = false;
  std::string lastAction = "Waiting for the computer";
  unsigned long lastRenderAt = 0;
  uint32_t filesDone = 0;
};
