#include "BleSyncService.h"

#include "BleClock.h"           // persist the phone's clock across deep-sleep (v3)
#include "BleProgressBridge.h"  // backfillUnclockedTimestamps at BLE-NTP clock set
#include "BleSyncProtocol.h"

#if ENABLE_BLE_SYNC

#include <Logging.h>        // firmware LOG_DBG / LOG_ERR
#include <NimBLEDevice.h>   // pin: h2zero/NimBLE-Arduino ^2.x  (API differs from 1.x!)
#include <esp_mac.h>
#include <sys/time.h>

#include <ctime>
#include <deque>
#include <mutex>

namespace proto = BleSyncProtocol;

namespace {
// Cap the inbound queue so a chatty/misbehaving peer can't grow heap unbounded
// on the RAM-tight C3. Oldest frames drop first.
constexpr size_t kMaxRxQueue = 32;
}  // namespace

// ---------------------------------------------------------------------------
// Impl — holds NimBLE handles + shared state. Callbacks run on the NimBLE host
// task, so every touch of shared state takes `mtx`.
// ---------------------------------------------------------------------------
struct BleSyncServiceImpl {
  std::string deviceId;
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* chCaps = nullptr;
  NimBLECharacteristic* chX4ToPhone = nullptr;
  NimBLECharacteristic* chPhoneToX4 = nullptr;
  NimBLECharacteristic* chSyncState = nullptr;

  bool running = false;
  bool advertising = false;
  bool connected = false;

  mutable std::mutex mtx;
  std::string pairedPhoneId;
  std::string lastPhoneResponse;
  std::string lastEvent;
  std::string lastAck;
  // v1 received PROGRESS (from phone):
  std::string rxDocument, rxTitleHash, rxXpointer;
  float rxPercentage = -1.0f;
  int64_t rxUpdatedAt = 0;
  bool rxHasProgress = false;
  int ackCount = 0;  // v3: phone ACKs this connection (reset on connect) — resend gating
  std::deque<proto::ParsedMessage> rxQueue;  // v2: FIFO of app messages for the reconcile driver
  BleSyncService::LogHandler logHandler;

  void log(const std::string& line) {
    LOG_DBG("BLESync", "%s", line.c_str());
    BleSyncService::LogHandler h;
    {
      std::lock_guard<std::mutex> lk(mtx);
      h = logHandler;
    }
    if (h) h(line);
  }

  // Push an app message onto the FIFO. Caller MUST hold mtx. Bounded: oldest drops.
  void enqueue(const proto::ParsedMessage& m) {
    if (rxQueue.size() >= kMaxRxQueue) rxQueue.pop_front();
    rxQueue.push_back(m);
  }

  void refreshSyncState() {
    std::string phone, evt, ack;
    bool paired;
    {
      std::lock_guard<std::mutex> lk(mtx);
      phone = pairedPhoneId;
      evt = lastEvent;
      ack = lastAck;
      paired = !pairedPhoneId.empty();
    }
    if (chSyncState) {
      const std::string v = proto::buildSyncState(paired, phone, evt, ack);
      chSyncState->setValue(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
  }

  void handleInbound(const std::string& json) {
    const proto::ParsedMessage m = proto::parseMessage(json);
    if (!m.ok) {
      log("inbound: unparseable payload dropped");
      return;
    }
    if (m.protocolVersion > proto::kProtocolVersion) {
      log("inbound: newer protocolVersion, dropped: " + m.type);
      return;
    }
    // BLE "NTP": set our clock from the phone's current time when we have none.
    if (m.now > 1000000000 && time(nullptr) < 1000000000) {
      struct timeval tv = {static_cast<time_t>(m.now), 0};
      settimeofday(&tv, nullptr);
      BleClock::writeFloor(m.now);  // v3: remember it across the next deep-sleep reset
      log("clock set from phone (BLE NTP)");
      // Positions read before the clock arrived carry an explicit 0 stamp
      // and would lose newest-wins forever. Stamp them "now" BEFORE this
      // session reconciles, so the phone sees them as fresh.
      const size_t fixed = BleProgress::backfillUnclockedTimestamps(static_cast<int64_t>(m.now));
      if (fixed > 0) {
        log("backfilled " + std::to_string(fixed) + " unclocked progress stamp(s)");
      }
    }
    if (m.type == proto::kTypePairAck || m.type == proto::kTypePairHello) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        pairedPhoneId = m.deviceId;
      }
      log("paired with phone: " + m.deviceId);
    } else if (m.type == proto::kTypeAck) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        lastPhoneResponse = "ACK for " + m.ackFor;
        lastAck = m.timestamp;
        ackCount++;  // v3: unblocks/short-circuits PROGRESS resend
      }
      log("phone ACK for " + m.ackFor);
    } else if (m.type == proto::kTypeDummyPosition) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "DUMMY_POSITION book=%s pct=%.3f",
                    m.dummyBookId.c_str(), m.dummyPercentage);
      {
        std::lock_guard<std::mutex> lk(mtx);
        lastPhoneResponse = buf;
        lastAck = m.timestamp;
      }
      log(std::string("phone ") + buf);
    } else if (m.type == proto::kTypeProgress) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        rxDocument = m.document;
        rxTitleHash = m.titleHash;
        rxXpointer = m.xpointer;
        rxPercentage = m.percentage;
        rxUpdatedAt = m.updatedAt;
        rxHasProgress = true;
        enqueue(m);
      }
      char buf[96];
      std::snprintf(buf, sizeof(buf), "PROGRESS %.1f%% doc=%.8s", m.percentage * 100, m.document.c_str());
      log(std::string("phone ") + buf);
    } else if (m.type == proto::kTypeManifest) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        enqueue(m);
      }
      log("phone MANIFEST (" + std::to_string(m.books.size()) + " books)");
    } else if (m.type == proto::kTypeWant) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        enqueue(m);
      }
      log("phone WANT (" + std::to_string(m.wantKeys.size()) + " keys)");
    } else {
      log("inbound: unhandled type " + m.type);
    }
    refreshSyncState();
  }
};

// ---------------------------------------------------------------------------
// NimBLE callbacks
// ---------------------------------------------------------------------------
namespace {

class ServerCb : public NimBLEServerCallbacks {
 public:
  explicit ServerCb(BleSyncServiceImpl* impl) : impl_(impl) {}

  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    {
      std::lock_guard<std::mutex> lk(impl_->mtx);
      impl_->connected = true;
      impl_->ackCount = 0;  // v3: fresh connection, fresh resend accounting
    }
    impl_->log("phone connected");
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    {
      std::lock_guard<std::mutex> lk(impl_->mtx);
      impl_->connected = false;
      impl_->rxQueue.clear();      // drop stale frames; next connection starts clean
      impl_->rxHasProgress = false;
    }
    impl_->log("phone disconnected (reason " + std::to_string(reason) + "); re-advertising");
    // Stay discoverable for the next rendezvous.
    NimBLEDevice::startAdvertising();
  }

 private:
  BleSyncServiceImpl* impl_;
};

class PhoneToX4Cb : public NimBLECharacteristicCallbacks {
 public:
  explicit PhoneToX4Cb(BleSyncServiceImpl* impl) : impl_(impl) {}

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue v = c->getValue();
    impl_->handleInbound(std::string(reinterpret_cast<const char*>(v.data()), v.length()));
  }

 private:
  BleSyncServiceImpl* impl_;
};

std::string deriveDeviceId() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  char out[20];
  std::snprintf(out, sizeof(out), "x4-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return std::string(out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
BleSyncService& BleSyncService::instance() {
  static BleSyncService s;
  return s;
}

void BleSyncService::begin(const std::string& deviceId) {
  if (!impl_) impl_ = new BleSyncServiceImpl();
  if (impl_->running) return;

  impl_->deviceId = deviceId.empty() ? deriveDeviceId() : deviceId;

  NimBLEDevice::init(proto::kAdvName);
  // A full PROGRESS JSON (doc + titleHash + xpointer) is ~300-350 B — far past the
  // 23 B default ATT MTU, which silently truncated the notify. Request a large MTU
  // so the whole message fits one notification (iOS negotiates the min of the two).
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(3 /* dBm; low duty for battery */);

  impl_->server = NimBLEDevice::createServer();
  impl_->server->setCallbacks(new ServerCb(impl_));  // NimBLE takes ownership

  NimBLEService* svc = impl_->server->createService(proto::kServiceUuid);

  impl_->chCaps = svc->createCharacteristic(proto::kCharCapabilities, NIMBLE_PROPERTY::READ);
  impl_->chX4ToPhone = svc->createCharacteristic(proto::kCharX4ToPhone, NIMBLE_PROPERTY::NOTIFY);
  impl_->chPhoneToX4 = svc->createCharacteristic(proto::kCharPhoneToX4, NIMBLE_PROPERTY::WRITE);
  impl_->chSyncState = svc->createCharacteristic(proto::kCharSyncState, NIMBLE_PROPERTY::READ);

  impl_->chPhoneToX4->setCallbacks(new PhoneToX4Cb(impl_));

  {
    const std::string caps = proto::buildCapabilities();
    impl_->chCaps->setValue(reinterpret_cast<const uint8_t*>(caps.data()), caps.size());
  }
  impl_->refreshSyncState();

  // NimBLE 2.x: services auto-start with the server; explicit start() is a no-op.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(proto::kServiceUuid);
  adv->setName(proto::kAdvName);
  adv->enableScanResponse(true);

  impl_->running = true;
  startAdvertising();
  impl_->log("BLE sync service started as " + impl_->deviceId);
}

void BleSyncService::stop() {
  if (!impl_ || !impl_->running) return;
  NimBLEDevice::stopAdvertising();
  // Full deinit frees the controller so Wi-Fi can reclaim the radio + heap.
  NimBLEDevice::deinit(true);
  impl_->running = false;
  impl_->advertising = false;
  impl_->connected = false;
  impl_->server = nullptr;
  impl_->chCaps = impl_->chX4ToPhone = impl_->chPhoneToX4 = impl_->chSyncState = nullptr;
  impl_->log("BLE sync service stopped (radio released)");
}

bool BleSyncService::isRunning() const { return impl_ && impl_->running; }

void BleSyncService::startAdvertising() {
  if (!isRunning()) return;
  NimBLEDevice::startAdvertising();
  impl_->advertising = true;
  impl_->log("advertising started");
}

void BleSyncService::stopAdvertising() {
  if (!isRunning()) return;
  NimBLEDevice::stopAdvertising();
  impl_->advertising = false;
  impl_->log("advertising stopped");
}

bool BleSyncService::isAdvertising() const { return impl_ && impl_->advertising; }

bool BleSyncService::isConnected() const {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->connected;
}

void BleSyncService::sendHasUpdate() {
  if (!isRunning()) return;
  const std::string payload = proto::buildHasUpdate(impl_->deviceId, /*dummyRevision=*/1);
  impl_->chX4ToPhone->setValue(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  const bool ok = impl_->chX4ToPhone->notify();
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->lastEvent = proto::kTypeHasUpdate;
  }
  impl_->refreshSyncState();
  impl_->log(ok ? "sent HAS_UPDATE" : "HAS_UPDATE notify skipped (no subscriber)");
}

void BleSyncService::sendNeedsUpdate() {
  if (!isRunning()) return;
  const std::string payload = proto::buildNeedsUpdate(impl_->deviceId, /*dummyBookId=*/"dummy-book");
  impl_->chX4ToPhone->setValue(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  const bool ok = impl_->chX4ToPhone->notify();
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->lastEvent = proto::kTypeNeedsUpdate;
  }
  impl_->refreshSyncState();
  impl_->log(ok ? "sent NEEDS_UPDATE" : "NEEDS_UPDATE notify skipped (no subscriber)");
}

void BleSyncService::sendProgress(const std::string& document, const std::string& titleHash,
                                  const std::string& xpointer, float percentage, int64_t updatedAt) {
  if (!isRunning()) return;
  const std::string payload =
      proto::buildProgress(impl_->deviceId, document, titleHash, xpointer, percentage, updatedAt);
  impl_->chX4ToPhone->setValue(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  const bool ok = impl_->chX4ToPhone->notify();
  impl_->log(ok ? "sent PROGRESS" : "PROGRESS notify skipped (no subscriber)");
}

bool BleSyncService::takeReceivedProgress(std::string& document, std::string& titleHash, std::string& xpointer,
                                          float& percentage, int64_t& updatedAt) {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (!impl_->rxHasProgress) return false;
  document = impl_->rxDocument;
  titleHash = impl_->rxTitleHash;
  xpointer = impl_->rxXpointer;
  percentage = impl_->rxPercentage;
  updatedAt = impl_->rxUpdatedAt;
  impl_->rxHasProgress = false;
  return true;
}

void BleSyncService::sendManifest(const std::vector<proto::ManifestEntry>& books, bool more) {
  if (!isRunning()) return;
  sendMessage(proto::buildManifest(impl_->deviceId, books, more, static_cast<int64_t>(time(nullptr))));
}

void BleSyncService::sendWant(const std::vector<std::string>& keys) {
  if (!isRunning() || keys.empty()) return;
  sendMessage(proto::buildWant(impl_->deviceId, keys));
}

void BleSyncService::sendMessage(const std::string& payload) {
  if (!isRunning() || payload.empty()) return;
  impl_->chX4ToPhone->setValue(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  const bool ok = impl_->chX4ToPhone->notify();
  impl_->log(ok ? "sent message" : "message notify skipped (no subscriber)");
}

bool BleSyncService::takeReceivedMessage(proto::ParsedMessage& out) {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (impl_->rxQueue.empty()) return false;
  out = std::move(impl_->rxQueue.front());
  impl_->rxQueue.pop_front();
  return true;
}

int BleSyncService::receivedAckCount() const {
  if (!impl_) return 0;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->ackCount;
}

std::string BleSyncService::pairedPhoneId() const {
  if (!impl_) return "";
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->pairedPhoneId;
}

std::string BleSyncService::lastPhoneResponse() const {
  if (!impl_) return "";
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->lastPhoneResponse;
}

std::string BleSyncService::lastEvent() const {
  if (!impl_) return "";
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->lastEvent;
}

void BleSyncService::setLogHandler(LogHandler handler) {
  if (!impl_) impl_ = new BleSyncServiceImpl();
  std::lock_guard<std::mutex> lk(impl_->mtx);
  impl_->logHandler = std::move(handler);
}

#else  // ENABLE_BLE_SYNC == 0 — compile to no-ops, no NimBLE dependency.

struct BleSyncServiceImpl {};
BleSyncService& BleSyncService::instance() { static BleSyncService s; return s; }
void BleSyncService::begin(const std::string&) {}
void BleSyncService::stop() {}
bool BleSyncService::isRunning() const { return false; }
void BleSyncService::startAdvertising() {}
void BleSyncService::stopAdvertising() {}
bool BleSyncService::isAdvertising() const { return false; }
bool BleSyncService::isConnected() const { return false; }
void BleSyncService::sendHasUpdate() {}
void BleSyncService::sendNeedsUpdate() {}
void BleSyncService::sendProgress(const std::string&, const std::string&, const std::string&, float, int64_t) {}
bool BleSyncService::takeReceivedProgress(std::string&, std::string&, std::string&, float&, int64_t&) {
  return false;
}
void BleSyncService::sendManifest(const std::vector<BleSyncProtocol::ManifestEntry>&, bool) {}
void BleSyncService::sendWant(const std::vector<std::string>&) {}
void BleSyncService::sendMessage(const std::string&) {}
bool BleSyncService::takeReceivedMessage(BleSyncProtocol::ParsedMessage&) { return false; }
int BleSyncService::receivedAckCount() const { return 0; }
std::string BleSyncService::pairedPhoneId() const { return ""; }
std::string BleSyncService::lastPhoneResponse() const { return ""; }
std::string BleSyncService::lastEvent() const { return ""; }
void BleSyncService::setLogHandler(LogHandler) {}

#endif  // ENABLE_BLE_SYNC
