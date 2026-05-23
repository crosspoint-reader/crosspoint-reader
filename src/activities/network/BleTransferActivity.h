#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "activities/Activity.h"

struct BleTransferRuntime;

class BleTransferActivity final : public Activity {
 public:
  enum class State {
    STARTING,
    ADVERTISING,
    CONNECTED,
    RECEIVING,
    VERIFYING,
    SAVED,
    FIRMWARE_CONFIRM,
    UPDATING,
    RESTARTING,
    SENDING,
    SENT,
    SAVE_HOST_PROMPT,
    FORGET_HOST_PROMPT,
    ERROR
  };
  enum class TransferKind { NONE, BOOK, BMP, FIRMWARE, CRASH_REPORT };

  explicit BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BleTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override {
    return state_ == State::VERIFYING || state_ == State::UPDATING || state_ == State::FIRMWARE_CONFIRM ||
           state_ == State::SENDING;
  }

  void enqueueBleConnected();
  void enqueueBleDisconnected();
  void enqueueControlWrite(const std::string& value);
  void enqueueDataWrite(const std::string& value);

 private:
  friend struct BleTransferRuntime;
  enum class BleEventType { CONNECTED, DISCONNECTED, CONTROL, DATA };
  struct BleEvent {
    BleEventType type;
    std::string value;
  };

  State state_ = State::STARTING;
  std::unique_ptr<BleTransferRuntime> ble_;
  FsFile uploadFile_;
  FsFile downloadFile_;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::deque<BleEvent> bleEvents_;
  size_t queuedBleEventBytes_ = 0;
  bool bleEventOverflow_ = false;

  std::string sessionCode_;
  std::string fileName_;
  std::string partPath_;
  std::string finalPath_;
  std::string expectedSha256_;
  std::string savedPath_;
  std::string errorMessage_;
  std::string deviceId_;
  std::string deviceNonce_;
  std::string trustedHostName_;
  std::string candidateHostId_;
  std::string candidateHostName_;
  std::string candidateHostSecret_;

  TransferKind transferKind_ = TransferKind::NONE;
  State pendingFinalState_ = State::CONNECTED;
  size_t expectedSize_ = 0;
  size_t receivedBytes_ = 0;
  size_t sentBytes_ = 0;
  size_t flashWrittenBytes_ = 0;
  size_t lastProgressStatusBytes_ = 0;
  size_t lastDisplayProgressBytes_ = 0;
  unsigned int lastFirmwareFlashRenderedPercent_ = 101;
  size_t uploadChunkSize_ = 0;
  size_t uploadAckBytes_ = 0;
  size_t downloadChunkSize_ = 0;
  uint32_t expectedSequence_ = 0;
  uint32_t downloadSequence_ = 0;
  uint32_t pendingDownloadAck_ = 0;
  bool helloAccepted_ = false;
  bool transferOpen_ = false;
  bool downloadOpen_ = false;
  bool downloadAwaitingAck_ = false;
  bool trustedHelloAccepted_ = false;
  bool hostPaired_ = false;
  bool hostPairSkipped_ = false;
  bool pendingCommit_ = false;
  bool statusDirty_ = true;
  bool removePartOnExit_ = false;
  bool uploadResumable_ = false;
  bool shaActive_ = false;
  mbedtls_sha256_context shaContext_;
  int promptSelection_ = 0;

  void enqueueBleEvent(BleEvent event);
  void processBleEvents();
  void onBleConnected();
  void onBleDisconnected();
  void onControlWrite(const std::string& value);
  void onDataWrite(const std::string& value);
  void processCommit();
  void startCrashReportDownload(size_t offset, size_t chunkSize);
  void pumpDownload();
  void resetTransfer(bool removePart);
  void setState(State state);
  void setError(const std::string& error);
  void publishStatus();
  std::string buildStatusJson() const;
  bool setPendingTrustedHost(const std::string& hostId, const std::string& hostName, const std::string& secret);
  void completeFinalState(State finalState);
  void handleFirmwareConfirm();
  void handleSaveHostPrompt();
  void handleForgetHostPrompt();
  void renderCompanionReady(const std::string& primary, const std::string& secondary) const;
  void renderFirmwareConfirm() const;
  void renderFirmwareUpdating() const;
  void renderSaveHostPrompt() const;
  void renderForgetHostPrompt() const;
};
