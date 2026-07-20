#include "NearbySyncRadio.h"

#ifdef SIMULATOR

#include <algorithm>
#include <utility>

NearbySyncRadio::~NearbySyncRadio() { stop(); }

bool NearbySyncRadio::start(void* context, const ReceiveCallback callback) {
#ifdef NEARBY_SYNC_TESTING
  if (!callback) return false;
  activated_ = true;
  started_ = true;
  callbackContext_ = context;
  callback_ = callback;
  return true;
#else
  (void)context;
  (void)callback;
  return false;
#endif
}

void NearbySyncRadio::stop() {
  callbackContext_ = nullptr;
  callback_ = nullptr;
  initialized_ = false;
  callbackRegistered_ = false;
  started_ = false;
#ifdef NEARBY_SYNC_TESTING
  testPackets_.clear();
#endif
}

bool NearbySyncRadio::addPeer(const NearbySync::MacAddress&) {
#ifdef NEARBY_SYNC_TESTING
  return started_;
#else
  return false;
#endif
}

bool NearbySyncRadio::send(const NearbySync::MacAddress& peerMac, const uint8_t* data, const size_t size) const {
#ifdef NEARBY_SYNC_TESTING
  if (!started_ || !data || size == 0 || size > NearbySync::MAX_PACKET_BYTES) return false;
  TestPacket packet;
  packet.destination = peerMac;
  packet.size = size;
  std::copy_n(data, size, packet.data.data());
  testPackets_.push_back(std::move(packet));
  return true;
#else
  (void)peerMac;
  (void)data;
  (void)size;
  return false;
#endif
}

void NearbySyncRadio::dispatchReceived(const uint8_t* sourceMac, const uint8_t* data, const size_t size) const {
#ifdef NEARBY_SYNC_TESTING
  if (started_ && callback_ && sourceMac && data && size > 0 && size <= NearbySync::MAX_PACKET_BYTES) {
    callback_(
        callbackContext_,
        NearbySync::MacAddress{sourceMac[0], sourceMac[1], sourceMac[2], sourceMac[3], sourceMac[4], sourceMac[5]},
        data, size);
  }
#else
  (void)sourceMac;
  (void)data;
  (void)size;
#endif
}

#ifdef NEARBY_SYNC_TESTING
bool NearbySyncRadio::takeTestPacket(TestPacket& packet) {
  if (testPackets_.empty()) return false;
  packet = std::move(testPackets_.front());
  testPackets_.erase(testPackets_.begin());
  return true;
}
#endif

#else

#include <Logging.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace {
constexpr char LOG_TAG[] = "CVNEAR";
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr NearbySync::MacAddress BROADCAST_MAC{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

NearbySyncRadio* activeRadio = nullptr;
SemaphoreHandle_t callbackGuard = nullptr;

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, const int length) {
  if (!callbackGuard || xSemaphoreTake(callbackGuard, 0) != pdTRUE) return;
  NearbySyncRadio* radio = activeRadio;
  if (radio && info && info->src_addr && data && length > 0) {
    // Keep the guard until enqueue returns. stop() takes the same guard before
    // clearing activeRadio, so an activity cannot delete its event queue while
    // a Wi-Fi task still holds its pointer.
    radio->dispatchReceived(info->src_addr, data, static_cast<size_t>(length));
  }
  xSemaphoreGive(callbackGuard);
}
}  // namespace

NearbySyncRadio::~NearbySyncRadio() { stop(); }

bool NearbySyncRadio::start(void* context, const ReceiveCallback callback) {
  if (started_) return true;
  if (!callback || (activeRadio && activeRadio != this)) return false;
  if (!callbackGuard) callbackGuard = xSemaphoreCreateMutex();
  if (!callbackGuard) return false;

  activated_ = true;
  callbackContext_ = context;
  callback_ = callback;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.setSleep(false);
  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
      esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
    LOG_ERR(LOG_TAG, "Could not configure ESP-NOW radio");
    stop();
    return false;
  }
  if (esp_now_init() != ESP_OK) {
    LOG_ERR(LOG_TAG, "Could not initialize ESP-NOW");
    stop();
    return false;
  }
  initialized_ = true;
  if (xSemaphoreTake(callbackGuard, portMAX_DELAY) != pdTRUE) {
    stop();
    return false;
  }
  activeRadio = this;
  xSemaphoreGive(callbackGuard);
  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
    LOG_ERR(LOG_TAG, "Could not register ESP-NOW receive callback");
    stop();
    return false;
  }
  callbackRegistered_ = true;
  if (!addPeer(BROADCAST_MAC)) {
    LOG_ERR(LOG_TAG, "Could not register ESP-NOW broadcast peer");
    stop();
    return false;
  }
  started_ = true;
  return true;
}

void NearbySyncRadio::stop() {
  // Serialize with the complete callback dispatch, not just its initial pointer
  // read. Once this guard is acquired, no Wi-Fi task can still call into the
  // exchange's queue, and future callbacks see activeRadio == nullptr.
  if (callbackGuard && xSemaphoreTake(callbackGuard, portMAX_DELAY) == pdTRUE) {
    if (activeRadio == this) activeRadio = nullptr;
    xSemaphoreGive(callbackGuard);
  }
  if (callbackRegistered_) {
    esp_now_unregister_recv_cb();
    callbackRegistered_ = false;
  }
  if (initialized_) {
    esp_now_deinit();
    initialized_ = false;
  }
  started_ = false;
  callbackContext_ = nullptr;
  callback_ = nullptr;
  if (activated_) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

bool NearbySyncRadio::addPeer(const NearbySync::MacAddress& peerMac) {
  if (!initialized_) return false;
  if (esp_now_is_peer_exist(peerMac.data())) return true;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, peerMac.data(), peerMac.size());
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

bool NearbySyncRadio::send(const NearbySync::MacAddress& peerMac, const uint8_t* data, const size_t size) const {
  return started_ && data && size > 0 && size <= NearbySync::MAX_PACKET_BYTES &&
         esp_now_send(peerMac.data(), data, size) == ESP_OK;
}

void NearbySyncRadio::dispatchReceived(const uint8_t* sourceMac, const uint8_t* data, const size_t size) const {
  if (!started_ || !callback_ || !sourceMac || !data || size == 0 || size > NearbySync::MAX_PACKET_BYTES) return;
  NearbySync::MacAddress source{};
  memcpy(source.data(), sourceMac, source.size());
  callback_(callbackContext_, source, data, size);
}

#endif
