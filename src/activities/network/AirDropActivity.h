#pragma once
#include <sdkconfig.h>
#if CROSSPOINT_AIRDROP
#include <BoardConfig.h>
#if !(FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC) || !FREEINK_CAP_USB_MSC || !defined(BOARD_HAS_PSRAM)
#error "AirDrop requires an X4 Pro or X4C profile with PSRAM and USB MSC enabled"
#endif
static_assert(CONFIG_IDF_TARGET_ESP32S3, "AirDrop requires ESP32-S3");
#include <ESP32Drop.h>
#include <HalStorage.h>
#include <airdrop/core/ad_cpio_stream.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class AirDropActivity final : public Activity {
 public:
  AirDropActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("AirDrop", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { Waiting, Asking, Receiving, Saved, TooLarge, Error };
  State state = State::Waiting;
  bool radioStarted = false, receiverStarted = false, leaving = false, popupNeedsUpdate = false;
  uint32_t startedAt = 0, lastStats = 0, observedErrors = 0, received = 0;
  OptionPopup popup;
  AdStatus stats{};
  uint32_t observedLimitRejections = 0;
  char savedPath[288]{}, temporaryPath[300]{}, statusLine[512]{};
  bool answerRequest(bool accept);
  void showState();
  void stop();
  static constexpr uint32_t RECEIVE_LIMIT = 30u * 1024u * 1024u;
  AdCpioStream archive;
  HalFile output;
  HalFile diagnostics;
  bool diagnosticsFailed = false;
  uint32_t diagnosticBytes = 0, lastDiagnostic = 0, lastProgressDraw = 0;
  void recordDiagnostics(bool force);
  bool temporaryOwned = false;
  uint32_t expectedSize = 0, writtenSize = 0, maxWriteMs = 0;
  bool beginFile(const char* name, uint32_t len);
  bool writeFile(const uint8_t* data, uint32_t len);
  bool endFile();
  void abortFile();
  static bool receiveStream(void* ctx, AdStreamEvent event, const uint8_t* data, uint32_t len);
};
#endif
