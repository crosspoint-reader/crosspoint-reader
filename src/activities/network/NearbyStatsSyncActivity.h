#pragma once

#include <array>
#include <string>

#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsCodec.h"
#include "network/NearbySyncExchange.h"

class NearbyStatsSyncActivity final : public Activity {
 public:
  NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override;

 private:
  NearbySync::MacAddress localMac_{};
  NearbySyncExchange exchange_;
  NearbySyncExchange::State lastExchangeState_ = NearbySyncExchange::State::Idle;
  ReadingStatsCodec::GlobalBytes localOffer_{};
  GlobalReadingStats localStats_{};
  GlobalReadingStats peerStats_{};
  std::string errorMessage_;
  bool localStatsTrusted_ = false;
  bool peerStatsReady_ = false;
  bool peerStatsRegresses_ = false;
  bool peerStatsStorageProtected_ = false;

  void startExchange(NearbySync::Role role);
  bool preparePeerStats();
  void inspectPeerStatsStorage();
  bool savePeerStats();
  void acceptPeerStats();
  void handleExchangeState();
  void setError(const std::string& message);
  void exitActivity();
};
