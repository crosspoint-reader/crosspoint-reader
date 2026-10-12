#pragma once

#include <functional>
#include <memory>
#include <string>

#include "ServerSession.h"
#include "activities/Activity.h"

enum class CalibreConnectState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * CalibreConnectActivity starts the file transfer server in STA mode,
 * but renders Calibre-specific instructions instead of the web transfer UI.
 */
class CalibreConnectActivity final : public Activity {
  CalibreConnectState state = CalibreConnectState::WIFI_SELECTION;

  // The server, its start, its requests, its link and its end (ServerSession.h).
  ServerSession session{renderer, mappedInput, "CAL"};
  std::string connectedIP;
  std::string connectedSSID;
  size_t lastProgressReceived = 0;
  size_t lastProgressTotal = 0;
  std::string currentUploadName;
  std::string lastCompleteName;
  unsigned long lastCompleteAt = 0;
  unsigned long lastProcessedCompleteAt = 0;  // Track which server value we've already processed
  std::string lastDeleteName;
  size_t lastDeleteCount = 0;
  unsigned long lastDeleteAt = 0;
  unsigned long lastProcessedDeleteAt = 0;
  int currentRssi = 0;
  unsigned long lastRssiUpdateTime = 0;
  // When the transfer progress was last painted; see PROGRESS_REPAINT_MS.
  unsigned long lastProgressPaintAt = 0;
  // The state of the link to the network the screen shows.
  WifiLinkWatch::Link shownLink = WifiLinkWatch::Link::Up;

  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();

 public:
  explicit CalibreConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CalibreConnect", renderer, mappedInput) {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return session.running(); }
  bool preventAutoSleep() override { return session.running(); }
};
