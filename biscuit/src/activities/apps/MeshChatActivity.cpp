#include "MeshChatActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

MeshChatActivity* MeshChatActivity::activeInstance = nullptr;

void MeshChatActivity::onEnter() {
  Activity::onEnter();
  state = CHAT_VIEW;
  messageCount = 0;
  messageHead = 0;
  scrollOffset = 0;
  peers.clear();
  peerSelectorIndex = 0;
  espnowInitialized = false;
  lastPresenceTime = 0;
  // MESH-002: reset pending render flag
  pendingRender = false;
  // MESH-003: reset dedup ring
  memset(recentHashes, 0, sizeof(recentHashes));
  dedupHead = 0;
  // MESH-004: reset relay queue
  relayHead = 0;
  relayCount = 0;
  activeInstance = this;
  if (!peersMux) peersMux = xSemaphoreCreateMutex();

  initEspNow();
  requestUpdate();
}

void MeshChatActivity::onExit() {
  Activity::onExit();
  deinitEspNow();
  activeInstance = nullptr;
  if (peersMux) { vSemaphoreDelete(peersMux); peersMux = nullptr; }
}

void MeshChatActivity::initEspNow() {
  RADIO.ensureWifi();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Set channel
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    LOG_ERR("MESH", "ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  // Add broadcast peer
  esp_now_peer_info_t broadcastPeer = {};
  memset(broadcastPeer.peer_addr, 0xFF, 6);
  broadcastPeer.channel = ESPNOW_CHANNEL;
  broadcastPeer.encrypt = false;
  if (esp_now_add_peer(&broadcastPeer) != ESP_OK) {
    LOG_ERR("MESH", "Failed to add broadcast peer");
  }

  WiFi.macAddress(localMac);
  espnowInitialized = true;
  LOG_DBG("MESH", "ESP-NOW initialized on channel %d", ESPNOW_CHANNEL);
}

void MeshChatActivity::deinitEspNow() {
  if (espnowInitialized) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    RADIO.shutdown();
    espnowInitialized = false;
  }
}

// FNV-1a 32-bit hash of sender MAC (6 bytes) + first 8 bytes of text payload
static uint32_t computeFrameHash(const uint8_t* mac, const uint8_t* textBytes) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) {
    h ^= mac[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 8; i++) {
    h ^= textBytes[i];
    h *= 16777619u;
  }
  return h;
}

void MeshChatActivity::onDataRecv(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
  if (!activeInstance || !recvInfo || len < 23) return;
  const uint8_t* mac = recvInfo->src_addr;

  uint8_t frameType = data[0];

  if (frameType == FRAME_CHAT) {
    char senderName[16] = {};
    char text[200] = {};
    memcpy(senderName, data + 7, 15);
    senderName[15] = '\0';
    int textLen = len - 23;
    if (textLen > 199) textLen = 199;
    memcpy(text, data + 23, textLen);
    text[199] = '\0';

    activeInstance->addMessage(mac, senderName, text, false);
    // MESH-002: do not call requestUpdate() from WiFi-task context; set flag instead
    activeInstance->pendingRender = true;

    // MESH-003 + MESH-004: relay with TTL, dedup, and safe out-of-callback send
    if (activeInstance->relayMode) {
      // Only relay frames not originated by us
      if (memcmp(data + 1, activeInstance->localMac, 6) != 0) {
        // MESH-003: read hop count — byte at offset 223 (only present in full frames)
        uint8_t hops = (len >= FRAME_CHAT_SIZE) ? data[FRAME_CHAT_SIZE - 1] : 0;
        if (hops < MAX_RELAY_HOPS) {
          // MESH-003: dedup — compute hash of (sender MAC + first 8 bytes of text)
          uint8_t textPad[8] = {};
          memcpy(textPad, data + 23, (len - 23 >= 8) ? 8 : len - 23);
          uint32_t h = computeFrameHash(data + 1, textPad);

          bool duplicate = false;
          portENTER_CRITICAL(&activeInstance->relayMux);
          for (int i = 0; i < DEDUP_RING_SIZE; i++) {
            if (activeInstance->recentHashes[i] == h) { duplicate = true; break; }
          }
          if (!duplicate && activeInstance->relayCount < RELAY_QUEUE_SIZE) {
            // Store hash in dedup ring
            activeInstance->recentHashes[activeInstance->dedupHead] = h;
            activeInstance->dedupHead = (activeInstance->dedupHead + 1) % DEDUP_RING_SIZE;

            // MESH-004: queue the relay frame (copy + increment hop count)
            int slot = (activeInstance->relayHead + activeInstance->relayCount) % RELAY_QUEUE_SIZE;
            // Zero the slot first so any bytes beyond len are clean, then copy
            memset(activeInstance->relayQueue[slot], 0, FRAME_CHAT_SIZE);
            int copyLen = (len < FRAME_CHAT_SIZE) ? len : FRAME_CHAT_SIZE;
            memcpy(activeInstance->relayQueue[slot], data, copyLen);
            // Ensure the frame is exactly FRAME_CHAT_SIZE bytes and bump hop count
            activeInstance->relayQueue[slot][FRAME_CHAT_SIZE - 1] = hops + 1;
            activeInstance->relayLen[slot] = FRAME_CHAT_SIZE;
            activeInstance->relayCount = activeInstance->relayCount + 1;
          }
          portEXIT_CRITICAL(&activeInstance->relayMux);
        }
      }
    }
  } else if (frameType == FRAME_PRESENCE) {
    char peerName[16] = {};
    memcpy(peerName, data + 7, 15);
    peerName[15] = '\0';

    if (activeInstance->peersMux &&
        xSemaphoreTake(activeInstance->peersMux, pdMS_TO_TICKS(10)) == pdTRUE) {
      bool found = false;
      for (auto& p : activeInstance->peers) {
        if (memcmp(p.mac, mac, 6) == 0) {
          memcpy(p.name, peerName, 16);
          p.lastSeen = millis();
          found = true;
          break;
        }
      }
      if (!found && activeInstance->peers.size() < 20) {
        Peer newPeer = {};
        memcpy(newPeer.mac, mac, 6);
        memcpy(newPeer.name, peerName, 16);
        newPeer.lastSeen = millis();
        activeInstance->peers.push_back(newPeer);
      }
      xSemaphoreGive(activeInstance->peersMux);
    }
  }
}

void MeshChatActivity::sendMessage(const char* text) {
  if (!espnowInitialized || !text || text[0] == '\0') return;

  // FRAME_CHAT_SIZE = 224: [0]=type [1-6]=MAC [7-22]=name [23-222]=text [223]=hop count
  uint8_t frame[FRAME_CHAT_SIZE] = {};
  frame[0] = FRAME_CHAT;
  memcpy(frame + 1, localMac, 6);
  strncpy(reinterpret_cast<char*>(frame + 7), localName, 15);
  strncpy(reinterpret_cast<char*>(frame + 23), text, 199);
  frame[FRAME_CHAT_SIZE - 1] = 0;  // hop count = 0 (original sender)

  uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, frame, sizeof(frame));

  addMessage(localMac, localName, text, true);
}

void MeshChatActivity::sendPresenceBeacon() {
  if (!espnowInitialized) return;

  uint8_t frame[23] = {};
  frame[0] = FRAME_PRESENCE;
  memcpy(frame + 1, localMac, 6);
  strncpy(reinterpret_cast<char*>(frame + 7), localName, 15);

  uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, frame, sizeof(frame));
  lastPresenceTime = millis();
}

void MeshChatActivity::addMessage(const uint8_t* senderMac, const char* senderName, const char* text, bool isLocal) {
  // MESH-001: protect the ring buffer — this may be called from the WiFi task
  portENTER_CRITICAL(&msgMux);
  int idx = messageHead;
  memcpy(messages[idx].senderMac, senderMac, 6);
  strncpy(messages[idx].senderName, senderName, 15);
  messages[idx].senderName[15] = '\0';
  strncpy(messages[idx].text, text, 199);
  messages[idx].text[199] = '\0';
  messages[idx].timestamp = millis();
  messages[idx].isLocal = isLocal;

  messageHead = (messageHead + 1) % MAX_MESSAGES;
  if (messageCount < MAX_MESSAGES) messageCount++;

  // Auto-scroll to newest
  scrollOffset = 0;
  portEXIT_CRITICAL(&msgMux);
}

void MeshChatActivity::loop() {
  // MESH-002: drain the render request flag set by the WiFi-task callback
  if (pendingRender) {
    pendingRender = false;
    requestUpdate();
  }

  // MESH-004: drain the relay queue — safe to call esp_now_send() from loop task
  if (espnowInitialized && relayCount > 0) {
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    portENTER_CRITICAL(&relayMux);
    while (relayCount > 0) {
      int slot = relayHead;
      int frameLen = relayLen[slot];
      uint8_t frameCopy[FRAME_CHAT_SIZE];
      memcpy(frameCopy, relayQueue[slot], frameLen);
      relayHead = (relayHead + 1) % RELAY_QUEUE_SIZE;
      relayCount = relayCount - 1;
      portEXIT_CRITICAL(&relayMux);

      esp_now_send(broadcast, frameCopy, frameLen);

      portENTER_CRITICAL(&relayMux);
    }
    portEXIT_CRITICAL(&relayMux);
  }

  // Send presence beacon periodically
  if (espnowInitialized && millis() - lastPresenceTime >= PRESENCE_INTERVAL_MS) {
    sendPresenceBeacon();
  }

  switch (state) {
    case CHAT_VIEW: {
      // Up/Down scroll through messages
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        if (scrollOffset < messageCount - 1) {
          scrollOffset++;
          requestUpdate();
        }
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        if (scrollOffset > 0) {
          scrollOffset--;
          requestUpdate();
        }
      }

      // Confirm -> compose message (keyboard)
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Message", "", 199),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& text = std::get<KeyboardResult>(result.data).text;
                if (!text.empty()) {
                  sendMessage(text.c_str());
                }
              }
              requestUpdate();
            });
      }

      // Right -> show peers
      if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        state = PEERS;
        peerSelectorIndex = 0;
        requestUpdate();
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;
    }

    case PEERS: {
      int peerCount = 0;
      if (peersMux && xSemaphoreTake(peersMux, pdMS_TO_TICKS(10)) == pdTRUE) {
        peerCount = static_cast<int>(peers.size());
        xSemaphoreGive(peersMux);
      }
      if (peerCount > 0) {
        buttonNavigator.onNext([this, peerCount] {
          peerSelectorIndex = ButtonNavigator::nextIndex(peerSelectorIndex, peerCount);
          requestUpdate();
        });
        buttonNavigator.onPrevious([this, peerCount] {
          peerSelectorIndex = ButtonNavigator::previousIndex(peerSelectorIndex, peerCount);
          requestUpdate();
        });
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = CHAT_VIEW;
        requestUpdate();
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        relayMode = !relayMode;
        requestUpdate();
      }
      break;
    }
  }
}

void MeshChatActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case CHAT_VIEW: renderChatView(); break;
    case PEERS: renderPeers(); break;
  }
  renderer.displayBuffer();
}

void MeshChatActivity::renderChatView() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  int peersCount = 0;
  if (peersMux && xSemaphoreTake(peersMux, pdMS_TO_TICKS(10)) == pdTRUE) {
    peersCount = static_cast<int>(peers.size());
    xSemaphoreGive(peersMux);
  }

  // MESH-001 / MESH-005: copy only visible messages under the spinlock.
  // The old approach copied the full ring buffer (Message[30] = ~6.8KB on the
  // stack), which overflows the 8KB FreeRTOS render task stack on ESP32-C3.
  // We now copy at most MAX_VISIBLE entries, cutting peak stack use to ~3.6KB.
  static constexpr int MAX_VISIBLE = 16;  // screen fits ~15 lines max
  Message visibleMsgs[MAX_VISIBLE];       // ~3.6KB instead of 6.8KB
  int visibleCount = 0;
  int localCount, localHead, localScroll;

  portENTER_CRITICAL(&msgMux);
  localCount  = messageCount;
  localHead   = messageHead;
  localScroll = scrollOffset;

  if (localCount > 0) {
    int maxDraw = localCount - localScroll;
    if (maxDraw > MAX_VISIBLE) maxDraw = MAX_VISIBLE;
    int msgIdx = (localHead - 1 - localScroll + MAX_MESSAGES) % MAX_MESSAGES;
    for (int i = 0; i < maxDraw; i++) {
      visibleMsgs[visibleCount++] = messages[msgIdx];
      msgIdx = (msgIdx - 1 + MAX_MESSAGES) % MAX_MESSAGES;
    }
  }
  portEXIT_CRITICAL(&msgMux);

  char subtitle[48];
  if (relayMode) {
    snprintf(subtitle, sizeof(subtitle), "%d msgs | %d peers | RELAY", localCount, peersCount);
  } else {
    snprintf(subtitle, sizeof(subtitle), "%d msgs | %d peers", localCount, peersCount);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Mesh Chat", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID) + 4;

  if (localCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, (contentTop + contentBottom) / 2,
                              "No messages yet. Press Confirm to send.");
  } else {
    // Render messages from newest (bottom) to oldest (top).
    // visibleMsgs[0] is the newest visible message; iterate forward to go older.
    int y = contentBottom - lineH;
    for (int drawn = 0; drawn < visibleCount && y >= contentTop; drawn++) {
      const Message& msg = visibleMsgs[drawn];

      char line[240];
      if (msg.isLocal) {
        snprintf(line, sizeof(line), "> %s", msg.text);
      } else {
        snprintf(line, sizeof(line), "%s: %s", msg.senderName, msg.text);
      }

      renderer.drawText(SMALL_FONT_ID, 16, y, line);
      y -= lineH;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "Send", "", "Peers >");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "^", "v");
}

void MeshChatActivity::renderPeers() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  std::vector<Peer> peersCopy;
  if (peersMux && xSemaphoreTake(peersMux, pdMS_TO_TICKS(10)) == pdTRUE) {
    peersCopy = peers;
    xSemaphoreGive(peersMux);
  }

  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "%d nearby", static_cast<int>(peersCopy.size()));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Peers", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (peersCopy.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              "No peers discovered yet...");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(peersCopy.size()), peerSelectorIndex,
        [&peersCopy](int index) -> std::string {
          return std::string(peersCopy[index].name);
        },
        [&peersCopy](int index) -> std::string {
          char buf[48];
          unsigned long ago = (millis() - peersCopy[index].lastSeen) / 1000;
          snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X  %lus ago",
                   peersCopy[index].mac[0], peersCopy[index].mac[1], peersCopy[index].mac[2],
                   peersCopy[index].mac[3], peersCopy[index].mac[4], peersCopy[index].mac[5], ago);
          return buf;
        });
  }

  char relayStr[24];
  snprintf(relayStr, sizeof(relayStr), "Relay: %s", relayMode ? "ON" : "OFF");
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding,
                    pageHeight - metrics.buttonHintsHeight - 22, relayStr);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Relay", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
