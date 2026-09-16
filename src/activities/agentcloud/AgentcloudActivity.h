#pragma once

#ifdef AGENTCLOUD_DASHBOARD

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include "AgentcloudProtocol.h"
#include "activities/Activity.h"

class AgentcloudBle;

class AgentcloudActivity final : public Activity {
  friend class AgentcloudBle;

 public:
  explicit AgentcloudActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~AgentcloudActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool honorsScreenInversion() const override { return false; }
  bool handleForcedRefresh() override {
    partialRefreshCount = 0;
    spinnerOnly = false;
    return false;
  }

 private:
  enum class ScreenState : uint8_t { Waiting, AuthError, BleError, Dashboard };

  void receiveFrame(uint16_t connectionHandle, const uint8_t* bytes, size_t length);
  void connectionOpened(uint16_t connectionHandle);
  void connectionClosed(uint16_t connectionHandle);
  void renderMessage(const char* message);
  void renderRow(size_t index, const agentcloud::Rect& rect);
  void renderStateOnly(size_t index, const agentcloud::Rect& rowRect);
  uint8_t drawWrappedText(int fontId, EpdFontFamily::Style style, int x, int y, int width, int lineStep,
                          uint8_t maxLines, const char* text, bool ink, bool centered = false);
  void drawStateIcon(agentcloud::CardState state, int x, int y, bool ink) const;
  void stopBle();
  void showError(ScreenState errorState);
  void logInternalHeap(const char* stage) const;

  AgentcloudBle* ble = nullptr;
  QueueHandle_t payloadQueue = nullptr;
  // Payload/snapshot workspaces are members so neither the BLE-host callback
  // nor the main task places a ~2 KB object on its stack.
  agentcloud::FrameAssembler frameAssembler{};
  agentcloud::PayloadMessage queuedPayload{};
  agentcloud::DashboardSnapshot dashboard{};
  agentcloud::DashboardSnapshot parsedDashboard{};
  std::atomic<bool> stopping{false};
  ScreenState screenState = ScreenState::Waiting;
  uint32_t lastSpinnerStepMs = 0;
  uint32_t lastAdvertisingCheckMs = 0;
  uint8_t dirtyRows = 0;
  uint8_t spinnerPhase = 0;
  uint8_t partialRefreshCount = 0;
  bool firstPaint = true;
  bool spinnerOnly = false;
  bool forceFullRefresh = false;
  // Largest wire text is 120 bytes; three UTF-8 ellipsis bytes plus NUL fit.
  char textLineScratch[agentcloud::MAX_HEADLINE_BYTES + 4]{};
};

#endif
