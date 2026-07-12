#pragma once
#include <cstdint>
#include <esp_now.h>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/portmacro.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MeshChatActivity final : public Activity {
 public:
  explicit MeshChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MeshChat", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State { CHAT_VIEW, PEERS };

  struct Message {
    uint8_t senderMac[6];
    char senderName[16];
    char text[200];
    unsigned long timestamp;
    bool isLocal;
  };

  struct Peer {
    uint8_t mac[6];
    char name[16];
    unsigned long lastSeen;
  };

  State state = CHAT_VIEW;
  ButtonNavigator buttonNavigator;

  // Messages (ring buffer using fixed array)
  static constexpr int MAX_MESSAGES = 30;
  Message messages[MAX_MESSAGES];
  int messageCount = 0;
  int messageHead = 0;
  int scrollOffset = 0;
  // MESH-001: spinlock protecting the message ring buffer (written from WiFi task,
  // read from render task). Must be mutable so const render methods can acquire it.
  mutable portMUX_TYPE msgMux = portMUX_INITIALIZER_UNLOCKED;

  // Peers
  std::vector<Peer> peers;
  int peerSelectorIndex = 0;
  SemaphoreHandle_t peersMux = nullptr;

  // Identity
  char localName[16] = "biscuit";
  uint8_t localMac[6] = {};

  // Relay mode: rebroadcast received CHAT frames from other nodes
  bool relayMode = false;

  // ESP-NOW state
  bool espnowInitialized = false;
  static constexpr uint8_t ESPNOW_CHANNEL = 1;
  unsigned long lastPresenceTime = 0;
  static constexpr unsigned long PRESENCE_INTERVAL_MS = 10000;

  // Protocol frame format:
  // [0]     = frame type: 0x01 = chat, 0x02 = presence
  // [1-6]   = sender MAC
  // [7-22]  = sender name (16 bytes, null-terminated)
  // [23-222]= text payload (200 bytes max, null-terminated)
  // [223]   = hop count (relay TTL, 0 = original sender)
  static constexpr uint8_t FRAME_CHAT = 0x01;
  static constexpr uint8_t FRAME_PRESENCE = 0x02;
  static constexpr int FRAME_CHAT_SIZE = 224;  // includes hop-count byte

  // MESH-003: relay TTL
  static constexpr uint8_t MAX_RELAY_HOPS = 3;

  // MESH-003: dedup ring buffer — FNV-1a hash of (sender MAC + first 8 bytes of text)
  static constexpr int DEDUP_RING_SIZE = 16;
  uint32_t recentHashes[DEDUP_RING_SIZE] = {};
  int dedupHead = 0;

  // MESH-002: flag set by the WiFi-task callback; drained in loop()
  volatile bool pendingRender = false;

  // MESH-004: relay queue — frames are queued in the callback and sent from loop()
  static constexpr int RELAY_QUEUE_SIZE = 4;
  uint8_t relayQueue[RELAY_QUEUE_SIZE][FRAME_CHAT_SIZE];
  int relayLen[RELAY_QUEUE_SIZE] = {};
  volatile int relayHead = 0;
  volatile int relayCount = 0;
  portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;

  void initEspNow();
  void deinitEspNow();
  void sendMessage(const char* text);
  void sendPresenceBeacon();
  void addMessage(const uint8_t* senderMac, const char* senderName, const char* text, bool isLocal);

  // ESP-NOW callbacks
  static void onDataRecv(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len);

  void renderChatView() const;
  void renderPeers() const;

  static MeshChatActivity* activeInstance;
};
