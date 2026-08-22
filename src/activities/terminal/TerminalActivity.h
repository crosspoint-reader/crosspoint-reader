#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "activities/Activity.h"
#include "terminal/TerminalDiagnostics.h"
#include "terminal/TerminalInput.h"
#include "terminal/TerminalParser.h"
#include "terminal/TerminalRenderGate.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWifi.h"

class TerminalActivity final : public Activity {
 public:
  explicit TerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Terminal", renderer, mappedInput), parser(screen), terminalInput(mappedInput, wifi) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool ownsPowerButton() const override { return terminalStarted; }
  bool skipLoopDelay() override { return terminalStarted; }

 private:
  static constexpr uint32_t INTERACTIVE_BATCH_MS = 8;
  static constexpr uint32_t MAX_BATCH_MS = 20;
  static constexpr uint32_t BURST_BOUNDARY_TIMEOUT_MS = 80;
  static constexpr size_t RX_CHUNK_BYTES = 128;
  static constexpr size_t RX_BUDGET_BYTES = 2048;
  static constexpr uint32_t RX_BUDGET_US = 2000;
#ifdef KNIETTY_ASYNC_WINDOW_PIPELINE
  static constexpr size_t ASYNC_WINDOW_CAPACITY = 8192;
  static constexpr uint8_t MAX_ASYNC_TAIL_REFRESHES = 4;
#endif
  // Battery voltage on the plain X4 comes from a noisy ADC. Sampling it in
  // every loop can redraw the whole header repeatedly and turn a small terminal
  // update into a full-frame fallback. A minute is ample for an E Ink status bar.
  static constexpr uint32_t BATTERY_STATUS_POLL_MS = 60000;
  static constexpr uint32_t EXIT_CONFIRM_MS = 3000;
  static constexpr uint32_t FORGET_CONFIRM_MS = 5000;
  static constexpr uint32_t PAIRING_NOTICE_MS = 3000;
  static constexpr uint8_t MAX_PAIRED_HOSTS = 4;
  static constexpr uint32_t DIAGNOSTIC_IDLE_TIMEOUT_MS = 30000;
  static constexpr uint32_t DIAGNOSTIC_WALL_TIMEOUT_MS = 180000;
  static constexpr uint16_t DIAGNOSTIC_COMMAND_LIMIT = 96;
  static constexpr uint16_t DIAGNOSTIC_ACTIVATION_LIMIT = 96;
#ifdef KNIETTY_ADAPTIVE_REFRESH
#ifndef KNIETTY_DISABLE_AUTO_SETTLE
  static constexpr uint32_t SETTLE_QUIET_MS = 250;
#endif
  static constexpr uint32_t CLEAN_QUIET_MS = 1000;
  static constexpr uint32_t CLEAN_DEBT_LIMIT = 80;
#endif
  static constexpr int HEADER_HEIGHT = 32;

  struct RefreshMetrics {
    uint32_t count = 0;
    uint32_t lastTotalUs = 0;
    uint32_t lastRenderUs = 0;
    uint32_t lastTransferUs = 0;
    uint32_t lastWaveformUs = 0;
    uint32_t lastQueueUs = 0;
    uint32_t lastLutUs = 0;
    uint32_t lastPlaneUs = 0;
    uint32_t lastBaselineUs = 0;
    uint16_t lastWindowWidth = 0;
    uint16_t lastWindowHeight = 0;
    uint32_t lastWindowBytes = 0;
    uint32_t minTotalUs = UINT32_MAX;
    uint32_t maxTotalUs = 0;
    uint64_t totalUs = 0;
    uint32_t windowedCount = 0;
    uint32_t fallbackCount = 0;
    uint32_t cleanCount = 0;
    uint32_t settleCount = 0;
    uint32_t rxBytes = 0;
    uint32_t rxReads = 0;
    uint32_t burstEnds = 0;
    uint32_t burstSnapshots = 0;
    uint32_t burstTimeouts = 0;
    uint32_t asyncTailUpdates = 0;

    void recordInteractive(uint32_t total, uint32_t render, uint32_t queue, const HalDisplay::RefreshTiming& timing,
                           bool windowed, uint16_t windowWidth, uint16_t windowHeight);
  };

  struct DiagnosticCommandState {
    knietty::diagnostics::Request request;
    uint32_t firstSequence = 0;
    uint32_t lastSequence = 0;
    uint32_t rxAtUs = 0;
    uint32_t parsedAtUs = 0;
    uint32_t queuedAtUs = 0;
    uint8_t coalesced = 1;
  };

#ifdef KNIETTY_ASYNC_WINDOW_PIPELINE
  struct AsyncRefreshJob {
    TerminalScreen::DirtyRegion dirtyRegion;
    uint32_t queuedAtMs = 0;
    uint32_t activationStartedAtMs = 0;
    uint32_t renderUs = 0;
    uint16_t windowWidth = 0;
    uint16_t windowHeight = 0;
  };
#endif

  enum class PairingNotice : uint8_t { None, ForgotAll, Revoked, StoreFailed };

  struct PairedHostDisplay {
    char name[33]{};
    char fingerprint[24]{};
  };

  mutable std::mutex modelMutex;
  TerminalScreen screen;
  TerminalScreen renderScreen;
  TerminalParser parser;
  TerminalWifi wifi;
  TerminalInput terminalInput;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Portrait;
  HalDisplay::FastRefreshProfile previousFastRefreshProfile = HalDisplay::FastRefreshProfile::PanelDefault;
  bool previousFadingFix = false;
  bool rendererStateCaptured = false;
  TerminalWifi::State displayState = TerminalWifi::State::Offline;
  TerminalWifi::Mode displayMode = TerminalWifi::Mode::Terminal;
  char displayClientName[33]{};
  char displayClientIp[16]{};
  char displayClock[6]{};
  uint16_t displayBattery = 101;
  char displayHostname[32]{};
  char displayLocalIp[16]{};
  char displayPairingCode[7]{};
  TerminalWifi::State renderDisplayState = TerminalWifi::State::Offline;
  TerminalWifi::Mode renderMode = TerminalWifi::Mode::Terminal;
  char renderClientName[33]{};
  char renderClientIp[16]{};
  char renderClock[6]{};
  uint16_t renderBattery = 0;
  char renderHostname[32]{};
  char renderLocalIp[16]{};
  char renderPairingCode[7]{};
  RefreshMetrics refreshMetrics;
  RefreshMetrics renderRefreshMetrics;
  PairedHostDisplay displayPairedHosts[MAX_PAIRED_HOSTS]{};
  PairedHostDisplay renderPairedHosts[MAX_PAIRED_HOSTS]{};
  uint8_t displayPairedHostCount = 0;
  uint8_t renderPairedHostCount = 0;
  uint8_t selectedPairedHost = 0;
  uint8_t renderSelectedPairedHost = 0;
  PairingNotice pairingNotice = PairingNotice::None;
  PairingNotice renderPairingNotice = PairingNotice::None;

  std::atomic<bool> contentDirty{false};
  std::atomic<bool> statusDirty{true};
  std::atomic<bool> clearContentArea{false};
  std::atomic<bool> exitConfirmationArmed{false};
  std::atomic<bool> waitingDiagnostics{false};
  std::atomic<bool> waitingPairedHosts{false};
  std::atomic<bool> diagnosticCommandQueued{false};
  std::atomic<bool> diagnosticEventReady{false};
  std::atomic<bool> runtimeControlActive{false};
  std::atomic<bool> forceTerminalRedraw{false};
  TerminalRenderGate renderGate;
  std::atomic<bool> forceFullRefresh{true};
#ifdef KNIETTY_ADAPTIVE_REFRESH
  std::atomic<bool> settleRequested{false};
  std::atomic<bool> cleanRequested{false};
  std::atomic<bool> settleDebtPending{false};
  std::atomic<uint32_t> cleanDebt{0};
#endif
  std::atomic<uint32_t> firstQueuedAt{0};
  std::atomic<uint32_t> lastQueuedAt{0};
  std::atomic<bool> burstBoundaryMode{false};
  std::atomic<bool> burstReady{false};
#ifdef KNIETTY_ASYNC_WINDOW_PIPELINE
  std::unique_ptr<uint8_t[]> asyncWindowBuffer;
  std::array<HalDisplay::PackedWindowRegion, TerminalScreen::ROWS> asyncWindowRegions{};
#endif
  uint32_t lastBatterySampleAt = 0;
  uint32_t lastNetworkGeneration = 0;
  uint32_t exitConfirmUntil = 0;
  uint32_t forgetConfirmUntil = 0;
  uint32_t revokeConfirmUntil = 0;
  uint32_t pairingNoticeUntil = 0;
  bool terminalStarted = false;
  bool firstRender = true;
  bool renderExitConfirmation = false;
  bool terminalInverted = false;
  bool renderInverted = false;
  bool framebufferInverted = false;
  bool renderWaitingDiagnostics = false;
  bool renderWaitingPairedHosts = false;
  bool forgetConfirmationArmed = false;
  bool renderForgetConfirmation = false;
  bool revokeConfirmationArmed = false;
  bool renderRevokeConfirmation = false;
  bool diagnosticPreviousInverted = false;
  DiagnosticCommandState diagnosticCommand;
  knietty::diagnostics::RefreshEvent diagnosticCompletedEvent;
  uint32_t diagnosticSessionStartedAt = 0;
  uint32_t diagnosticLastActivityAt = 0;
  uint16_t diagnosticCommandCount = 0;
  uint16_t diagnosticActivationCount = 0;
#ifdef KNIETTY_ADAPTIVE_REFRESH
  TerminalScreen::DirtyRegion settleRegion;
#endif

  void startTerminal();
  void pollWifi(uint32_t now);
  void pollTerminalControl(uint32_t now);
  void pollDiagnostics(uint32_t now);
  bool sendCompletedRefreshEvents(uint32_t now);
  void resetDiagnostics(uint32_t now);
  void abortDiagnostics();
  bool sendDiagnosticStatus(uint32_t sequence, knietty::diagnostics::Command command,
                            knietty::diagnostics::Status status, knietty::diagnostics::Error error);
  bool sendDiagnosticSessionInfo(uint32_t sequence);
  bool sendRefreshMetrics(uint32_t sequence);
  void syncNetworkState();
  void refreshPairedHosts();
  void setPairingNotice(PairingNotice notice, uint32_t now);
  void syncClock(uint32_t now);
  bool handlePowerButton(uint32_t now);
  void toggleInversion();
  void scheduleRender(bool forceFull);
  void drawStatus();
  void drawWaitingScreen();
  void drawPairedHosts();
  void drawRefreshDiagnostics();
  void drawApprovalPrompt();
  void drawContextualHints(const MappedInputManager::Labels& labels);
  void drawDirtyCells(const TerminalScreen::DirtyRegion& dirtyRegion);
#ifdef KNIETTY_ASYNC_WINDOW_PIPELINE
  bool startAsyncTerminalRefresh(const TerminalScreen::DirtyRegion& dirtyRegion, uint32_t queuedAtMs, uint32_t renderUs,
                                 AsyncRefreshJob& job);
  bool composeAsyncPending(TerminalScreen::DirtyRegion& pendingRegion, uint32_t& queuedAtMs, uint32_t& renderUs);
  void completeAsyncRefresh(const AsyncRefreshJob& job);
  void runAsyncTerminalPipeline(AsyncRefreshJob job);
#endif
  const char* pairingNoticeText(PairingNotice notice) const;
};
