#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SsidChannelActivity final : public Activity {
 public:
  explicit SsidChannelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SsidChannel", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SENDING || state == RECEIVING; }

 private:
  enum State { MODE_SELECT, SEND_COMPOSE, SENDING, RECEIVING, MESSAGE_LIST };
  State state = MODE_SELECT;
  ButtonNavigator buttonNavigator;
  int modeIndex = 0;  // 0=send, 1=receive

  // Send
  std::string outMessage;
  std::string broadcastSsid;
  unsigned long sendStart = 0;
  static constexpr unsigned long SEND_DURATION_MS = 30000;
  static constexpr const char* SSID_PREFIX = "BC_";

  // Receive
  struct ReceivedMsg { std::string text; int8_t rssi; unsigned long timestamp; };
  std::vector<ReceivedMsg> received;
  static constexpr int MAX_RECEIVED = 20;
  int msgIndex = 0;
  bool scanning = false;
  unsigned long lastScan = 0;
  static constexpr unsigned long SCAN_INTERVAL_MS = 5000;

  // Base64 encode/decode (minimal, for SSID payload)
  static std::string base64Encode(const std::string& input);
  static std::string base64Decode(const std::string& input);

  void startSending();
  void stopSending();
  void startReceiving();
  void doScan();

  void renderModeSelect() const;
  void renderSending() const;
  void renderReceiving() const;
};
