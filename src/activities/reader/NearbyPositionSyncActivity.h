#pragma once

#include <Epub.h>

#include <array>
#include <memory>
#include <string>

#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "network/NearbySyncExchange.h"

struct NearbyReaderLayout {
  int fontId = 0;
  float lineCompression = 0.0F;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
};

class NearbyPositionSyncActivity final : public Activity {
 public:
  NearbyPositionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                             CrossPointPosition localPosition, SavedProgressPosition localSavedPosition,
                             std::string localChapterName, NearbyReaderLayout localLayout);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override;
  bool isReaderActivity() const override { return true; }

 private:
  std::shared_ptr<Epub> epub_;
  CrossPointPosition localPosition_{};
  SavedProgressPosition localSavedPosition_{};
  std::string localChapterName_;
  NearbyReaderLayout localLayout_{};
  std::string documentFingerprint_;
  NearbySync::MacAddress localMac_{};
  NearbySyncExchange exchange_;
  NearbySyncExchange::State lastExchangeState_ = NearbySyncExchange::State::Idle;
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> localOfferBytes_{};
  size_t localOfferSize_ = 0;
  NearbySync::PositionOffer peerOffer_{};
  CrossPointPosition peerPosition_{};
  std::string peerChapterName_;
  std::string errorMessage_;
  bool preparing_ = false;
  bool peerPositionReady_ = false;

  bool prepareExchange(NearbySync::Role role);
  bool preparePeerPosition();
  void startExchange(NearbySync::Role role);
  void applyPeerPosition();
  void handleExchangeState();
  void setError(const std::string& message);
  void exitActivity();
};
