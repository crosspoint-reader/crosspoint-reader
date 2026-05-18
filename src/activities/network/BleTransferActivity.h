#pragma once

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include <cstdint>
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
    INSTALLING,
    INSTALLED,
    SAVED,
    SENDING,
    SENT,
    SAVE_HOST_PROMPT,
    FORGET_HOST_PROMPT,
    ERROR
  };
  enum class TransferKind { NONE, PACKAGE, BOOK, CRASH_REPORT, PACKAGE_STATE };

  explicit BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BleTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override {
    return state_ == State::VERIFYING || state_ == State::INSTALLING || state_ == State::SENDING;
  }

  void onBleConnected();
  void onBleDisconnected();
  void onControlWrite(const std::string& value);
  void onDataWrite(const std::string& value);

 private:
  friend struct BleTransferRuntime;

  State state_ = State::STARTING;
  std::unique_ptr<BleTransferRuntime> ble_;
  FsFile uploadFile_;
  FsFile downloadFile_;

  std::string sessionCode_;
  std::string fileName_;
  std::string partPath_;
  std::string finalPath_;
  std::string expectedSha256_;
  std::string packageId_;
  std::string packageName_;
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
  size_t lastProgressStatusBytes_ = 0;
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
  bool shaActive_ = false;
  mbedtls_sha256_context shaContext_;
  int promptSelection_ = 0;

  void processCommit();
  void startCrashReportDownload();
  void startPackageStateDownload(const std::string& packageId);
  void pumpDownload();
  void resetTransfer(bool removePart);
  void setState(State state);
  void setError(const std::string& error);
  void publishStatus();
  std::string buildStatusJson() const;
  void completeFinalState(State finalState);
  void handleSaveHostPrompt();
  void handleForgetHostPrompt();
  void renderSaveHostPrompt() const;
  void renderForgetHostPrompt() const;
};
