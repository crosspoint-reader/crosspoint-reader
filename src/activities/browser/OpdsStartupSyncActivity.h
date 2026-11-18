#pragma once
#include <OpdsClient.h>

#include <string>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "network/OpdsBatchDownload.h"
#include "network/OpdsHttpTransport.h"

/**
 * Boot-time OPDS sync (SETTINGS.opdsSyncOnStartup). Joins a saved WiFi network,
 * downloads whatever the first saved server's catalog has that the SD card does
 * not, then continues to the home screen.
 *
 * Every failure is non-fatal and quick: no saved network, no saved server, a
 * connect that does not land inside CONNECT_TIMEOUT_MS, or an unreachable
 * server all just fall through to home. Back cancels at any point.
 */
class OpdsStartupSyncActivity final : public Activity, private UiAppHost {
 public:
  OpdsStartupSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // True when a startup sync is worth entering: enabled, at least one saved
  // OPDS server, and at least one saved WiFi network. Call after the stores are
  // loaded.
  static bool shouldRun();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class SyncState { CONNECTING, SYNCING };

  // A boot must not stall on a network that is not there.
  static constexpr unsigned long CONNECT_TIMEOUT_MS = 12000;

  SyncState state = SyncState::CONNECTING;
  OpdsServer server;
  unsigned long connectStartedAt = 0;
  bool cancelled = false;

  std::string statusMessage;
  OpdsBatchDownload::Status status;
  OpdsBatchDownload::Status shown;  // Copy of status for the render task
  size_t bytes = 0;       // Bytes of the current transfer
  size_t bytesTotal = 0;  // Content-Length of the current transfer, 0 if unknown
  // Same client setup as the OPDS browser (credentials, custom headers, saved
  // tokens). The transport must outlive the client, so it is declared first.
  OpdsHttpTransport opdsTransport;
  freeink::opds::OpdsClient opdsClient{opdsTransport};
  int renderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  static void rootScreen(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static bool onProgress(void* ctx, const OpdsBatchDownload::Status& progress);
  static HttpDownloader::DownloadError onTransfer(void* ctx, const std::string& url, const std::string& dest,
                                                  const freeink::opds::HttpAuth& auth);
  // Pumps input and repaints at most every PROGRESS_STEP_PERCENT / interval.
  void pumpProgress();

  bool startWifi();
  void runSync();
  bool preventAutoSleep() override { return true; }
};
