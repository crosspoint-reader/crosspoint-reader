#include "BleTransferActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "BleTrustedHostStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "marginalia/PackageArchiveInstaller.h"
#include "marginalia/PackageStore.h"

namespace {

constexpr const char* BLE_DEVICE_NAME = "Marginalia Transfer";
constexpr const char* BLE_SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BOOKS_ROOT = "/Books";
constexpr const char* CRASH_REPORT_PATH = "/crash_report.txt";
constexpr const char* CRASH_REPORT_NAME = "crash_report.txt";
constexpr size_t MAX_BLE_PACKAGE_BYTES = 4UL * 1024UL * 1024UL;
constexpr size_t MAX_BLE_BOOK_BYTES = 32UL * 1024UL * 1024UL;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES = 160;
constexpr size_t MAX_FILENAME_BYTES = 96;
constexpr size_t BLE_HOST_ID_MAX_BYTES = 64;
constexpr size_t BLE_HOST_NAME_MAX_BYTES = 48;
constexpr size_t BLE_SHARED_SECRET_HEX_BYTES = 64;
constexpr size_t BLE_NONCE_BYTES = 16;
constexpr size_t BLE_PROGRESS_STATUS_INTERVAL_BYTES = 4UL * 1024UL;
constexpr size_t MPKG_SUFFIX_LEN = 9;
constexpr size_t EPUB_SUFFIX_LEN = 5;

std::string makeSessionCode() {
  char buffer[7];
  snprintf(buffer, sizeof(buffer), "%06u", static_cast<unsigned>(esp_random() % 1000000UL));
  return buffer;
}

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string makeDeviceId() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  return bytesToHex(mac, sizeof(mac));
}

std::string makeNonceHex() {
  uint8_t nonce[BLE_NONCE_BYTES] = {};
  for (auto& byte : nonce) byte = static_cast<uint8_t>(esp_random() & 0xFF);
  return bytesToHex(nonce, sizeof(nonce));
}

std::string toLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool isHexString(const std::string& value, const size_t length) {
  if (value.length() != length) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool isHexSha256(const std::string& value) { return isHexString(value, 64); }

bool endsWithMpkgZip(const std::string& value) {
  constexpr const char* suffix = ".mpkg.zip";
  if (value.length() < MPKG_SUFFIX_LEN) return false;
  return toLowerAscii(value.substr(value.length() - MPKG_SUFFIX_LEN)) == suffix;
}

bool endsWithEpub(const std::string& value) {
  constexpr const char* suffix = ".epub";
  if (value.length() < EPUB_SUFFIX_LEN) return false;
  return toLowerAscii(value.substr(value.length() - EPUB_SUFFIX_LEN)) == suffix;
}

bool isSafeBleFileName(const std::string& value) {
  if (value.empty() || value.length() > MAX_FILENAME_BYTES || value[0] == '.') return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool isSafeHostId(const std::string& value) {
  if (value.empty() || value.length() > BLE_HOST_ID_MAX_BYTES) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '-' || c == '_';
  });
}

std::string sanitizeHostName(std::string value) {
  if (value.empty()) return "Trusted host";
  if (value.length() > BLE_HOST_NAME_MAX_BYTES) value.resize(BLE_HOST_NAME_MAX_BYTES);
  for (char& c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 32 || uc > 126) c = '?';
  }
  return value;
}

bool isSafeBlePackageName(const std::string& value) { return isSafeBleFileName(value) && endsWithMpkgZip(value); }

bool isSafeBleBookName(const std::string& value) { return isSafeBleFileName(value) && endsWithEpub(value); }

std::string transferKindName(const BleTransferActivity::TransferKind kind) {
  switch (kind) {
    case BleTransferActivity::TransferKind::PACKAGE:
      return "package";
    case BleTransferActivity::TransferKind::BOOK:
      return "book";
    case BleTransferActivity::TransferKind::CRASH_REPORT:
      return "crash_report";
    case BleTransferActivity::TransferKind::NONE:
      return "";
  }
  return "";
}

std::string sha256ToHex(const uint8_t digest[32]) { return bytesToHex(digest, 32); }

std::string trustedHostMessage(const std::string& nonce, const std::string& hostId) {
  return nonce + "|" + hostId + "|1";
}

std::string hmacSha256Hex(const std::string& secret, const std::string& message) {
  uint8_t output[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return "";
  const int ret = mbedtls_md_hmac(md, reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
                                  reinterpret_cast<const uint8_t*>(message.data()), message.size(), output);
  if (ret != 0) return "";
  return bytesToHex(output, sizeof(output));
}

std::string stateName(BleTransferActivity::State state) {
  switch (state) {
    case BleTransferActivity::State::STARTING:
      return "starting";
    case BleTransferActivity::State::ADVERTISING:
      return "advertising";
    case BleTransferActivity::State::CONNECTED:
      return "connected";
    case BleTransferActivity::State::RECEIVING:
      return "receiving";
    case BleTransferActivity::State::VERIFYING:
      return "verifying";
    case BleTransferActivity::State::INSTALLING:
      return "installing";
    case BleTransferActivity::State::INSTALLED:
      return "installed";
    case BleTransferActivity::State::SAVED:
      return "saved";
    case BleTransferActivity::State::SENDING:
      return "sending";
    case BleTransferActivity::State::SENT:
      return "sent";
    case BleTransferActivity::State::SAVE_HOST_PROMPT:
      return "save_host_prompt";
    case BleTransferActivity::State::FORGET_HOST_PROMPT:
      return "forget_host_prompt";
    case BleTransferActivity::State::ERROR:
      return "error";
  }
  return "unknown";
}

uint32_t readLe32(const std::string& value) {
  const auto* b = reinterpret_cast<const uint8_t*>(value.data());
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 120);
    server->setDataLen(connInfo.getConnHandle(), 251);
    activity_.onBleConnected();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { activity_.onBleDisconnected(); }

 private:
  BleTransferActivity& activity_;
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.onControlWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit DataCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.onDataWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

}  // namespace

struct BleTransferRuntime {
  explicit BleTransferRuntime(BleTransferActivity& activity)
      : serverCallbacks(activity), controlCallbacks(activity), dataCallbacks(activity) {}

  NimBLEServer* server = nullptr;
  NimBLEService* service = nullptr;
  NimBLECharacteristic* status = nullptr;
  NimBLECharacteristic* dataOut = nullptr;
  ServerCallbacks serverCallbacks;
  ControlCallbacks controlCallbacks;
  DataCallbacks dataCallbacks;

  bool begin(BleTransferActivity& activity) {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks, false);

    service = server->createService(BLE_SERVICE_UUID);
    if (!service) return false;

    auto* control = service->createCharacteristic(BLE_CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    auto* dataIn = service->createCharacteristic(BLE_DATA_IN_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    status = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    dataOut = service->createCharacteristic(BLE_DATA_OUT_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (!control || !dataIn || !status || !dataOut) return false;

    control->setCallbacks(&controlCallbacks);
    dataIn->setCallbacks(&dataCallbacks);
    status->setValue(activity.buildStatusJson());

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(BLE_DEVICE_NAME);
    advertising->start();
    return true;
  }

  void publish(const std::string& json) {
    if (!status) return;
    status->setValue(json);
    status->notify();
  }

  void notifyData(const uint8_t* data, const size_t length) {
    if (!dataOut) return;
    dataOut->setValue(data, length);
    dataOut->notify();
  }

  void end() {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    server = nullptr;
    service = nullptr;
    status = nullptr;
    dataOut = nullptr;
  }
};

BleTransferActivity::BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTransfer", renderer, mappedInput) {}

BleTransferActivity::~BleTransferActivity() = default;

void BleTransferActivity::onEnter() {
  Activity::onEnter();
  sessionCode_ = makeSessionCode();
  deviceId_ = makeDeviceId();
  deviceNonce_ = makeNonceHex();
  {
    RenderLock lock(*this);
    BLE_TRUSTED_HOSTS.loadFromFile();
  }
  mbedtls_sha256_init(&shaContext_);
  setState(State::STARTING);

  ble_ = std::make_unique<BleTransferRuntime>(*this);
  if (!ble_->begin(*this)) {
    setError("Could not start BLE");
    return;
  }

  setState(State::ADVERTISING);
  publishStatus();
}

void BleTransferActivity::onExit() {
  Activity::onExit();
  resetTransfer(true);
  if (ble_) {
    ble_->end();
    ble_.reset();
  }
  mbedtls_sha256_free(&shaContext_);
}

void BleTransferActivity::loop() {
  if (state_ == State::SAVE_HOST_PROMPT) {
    handleSaveHostPrompt();
    return;
  }

  if (state_ == State::FORGET_HOST_PROMPT) {
    handleForgetHostPrompt();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && BLE_TRUSTED_HOSTS.hasHosts() &&
      (state_ == State::ADVERTISING || state_ == State::CONNECTED)) {
    promptSelection_ = 0;
    setState(State::FORGET_HOST_PROMPT);
    return;
  }

  if (pendingCommit_) {
    pendingCommit_ = false;
    processCommit();
    return;
  }

  if (state_ == State::SENDING && downloadOpen_) {
    if (statusDirty_) publishStatus();
    pumpDownload();
    return;
  }

  if (statusDirty_) {
    publishStatus();
  }
}

void BleTransferActivity::onBleConnected() {
  helloAccepted_ = false;
  trustedHelloAccepted_ = false;
  trustedHostName_.clear();
  setState(State::CONNECTED);
}

void BleTransferActivity::onBleDisconnected() {
  if (transferOpen_ || downloadOpen_) {
    setError("client disconnected");
    resetTransfer(true);
    return;
  }
  helloAccepted_ = false;
  setState(State::ADVERTISING);
}

void BleTransferActivity::onControlWrite(const std::string& value) {
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, value.data(), value.size());
  if (parseError) {
    setError("invalid control JSON");
    return;
  }

  const std::string op = doc["op"] | "";
  if (op == "hello") {
    const int version = doc["version"] | 0;
    const std::string code = doc["code"] | "";
    const std::string hostId = doc["host_id"] | "";
    const std::string response = toLowerAscii(doc["response"] | "");
    if (version != 1) {
      setError("unsupported protocol version");
      return;
    }

    if (!hostId.empty() && !response.empty()) {
      if (!isSafeHostId(hostId) || !isHexString(response, 64)) {
        setError("invalid trusted host auth");
        return;
      }
      const BleTrustedHost* host = BLE_TRUSTED_HOSTS.findHost(hostId);
      if (!host) {
        setError("unknown trusted host");
        return;
      }
      const std::string expected = hmacSha256Hex(host->secret, trustedHostMessage(deviceNonce_, hostId));
      if (expected.empty() || expected != response) {
        setError("invalid trusted host auth");
        return;
      }
      helloAccepted_ = true;
      trustedHelloAccepted_ = true;
      trustedHostName_ = host->name.empty() ? hostId : host->name;
      deviceNonce_ = makeNonceHex();
      setState(State::CONNECTED);
      return;
    }

    if (code != sessionCode_) {
      setError("invalid session code");
      return;
    }

    const std::string candidateHostId = doc["pair_host_id"] | "";
    const std::string candidateHostName = doc["pair_host_name"] | "";
    const std::string candidateSecret = toLowerAscii(doc["pair_secret"] | "");
    candidateHostId_.clear();
    candidateHostName_.clear();
    candidateHostSecret_.clear();
    if (!candidateHostId.empty() || !candidateSecret.empty()) {
      if (!isSafeHostId(candidateHostId) || !isHexString(candidateSecret, BLE_SHARED_SECRET_HEX_BYTES)) {
        setError("invalid trusted host setup");
        return;
      }
      candidateHostId_ = candidateHostId;
      candidateHostName_ = sanitizeHostName(candidateHostName);
      candidateHostSecret_ = candidateSecret;
    }

    helloAccepted_ = true;
    trustedHelloAccepted_ = false;
    setState(State::CONNECTED);
    return;
  }

  if (!helloAccepted_) {
    setError("session code required");
    return;
  }

  if (op == "start_put") {
    resetTransfer(true);

    const std::string kind = doc["kind"] | "";
    fileName_ = doc["name"] | "";
    expectedSize_ = doc["size"] | 0;
    expectedSha256_ = toLowerAscii(doc["sha256"] | "");
    transferKind_ = TransferKind::NONE;

    if (!isHexSha256(expectedSha256_)) {
      setError("invalid sha256");
      return;
    }
    if (kind == "package") {
      if (!isSafeBlePackageName(fileName_)) {
        setError("unsafe package filename");
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_PACKAGE_BYTES) {
        setError("invalid package size");
        return;
      }
      if (!Marginalia::ensurePackageBaseDirectories()) {
        setError("could not create package directories");
        return;
      }
      transferKind_ = TransferKind::PACKAGE;
      partPath_ = std::string(Marginalia::PACKAGE_SIDELOAD_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(Marginalia::PACKAGE_SIDELOAD_ROOT) + "/" + fileName_;
    } else if (kind == "book") {
      if (!isSafeBleBookName(fileName_)) {
        setError("unsafe book filename");
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BOOK_BYTES) {
        setError("invalid book size");
        return;
      }
      if (!Storage.exists(BOOKS_ROOT) && !Storage.mkdir(BOOKS_ROOT)) {
        setError("could not create books directory");
        return;
      }
      transferKind_ = TransferKind::BOOK;
      partPath_ = std::string(BOOKS_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(BOOKS_ROOT) + "/" + fileName_;
      if (Storage.exists(finalPath_.c_str())) {
        setError("exists");
        return;
      }
    } else {
      setError("unsupported transfer kind");
      return;
    }
    if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
    if (!Storage.openFileForWrite("BLE", partPath_, uploadFile_)) {
      setError("could not open transfer file");
      return;
    }

    mbedtls_sha256_starts(&shaContext_, 0);
    shaActive_ = true;
    receivedBytes_ = 0;
    expectedSequence_ = 0;
    transferOpen_ = true;
    removePartOnExit_ = true;
    setState(State::RECEIVING);
    return;
  }

  if (op == "start_get") {
    resetTransfer(true);

    const std::string kind = doc["kind"] | "";
    if (kind != "crash_report") {
      setError("unsupported transfer kind");
      return;
    }

    startCrashReportDownload();
    return;
  }

  if (op == "get_ack") {
    if (!downloadOpen_ || !downloadAwaitingAck_) {
      setError("no download pending");
      return;
    }
    const uint32_t sequence = doc["sequence"] | UINT32_MAX;
    if (sequence != pendingDownloadAck_) {
      setError("unexpected download ack");
      return;
    }
    downloadAwaitingAck_ = false;
    statusDirty_ = true;
    requestUpdate();
    return;
  }

  if (op == "commit") {
    if (!transferOpen_) {
      setError("no transfer open");
      return;
    }
    pendingCommit_ = true;
    setState(State::VERIFYING);
    return;
  }

  if (op == "cancel") {
    resetTransfer(true);
    setState(State::CONNECTED);
    return;
  }

  setError("unknown control op");
}

void BleTransferActivity::onDataWrite(const std::string& value) {
  if (!transferOpen_ || state_ != State::RECEIVING) return;
  if (value.size() <= sizeof(uint32_t)) {
    setError("invalid data frame");
    resetTransfer(true);
    return;
  }

  const uint32_t sequence = readLe32(value);
  if (sequence != expectedSequence_) {
    setError("unexpected data sequence");
    resetTransfer(true);
    return;
  }

  const uint8_t* payload = reinterpret_cast<const uint8_t*>(value.data() + sizeof(uint32_t));
  const size_t payloadSize = value.size() - sizeof(uint32_t);
  if (receivedBytes_ + payloadSize > expectedSize_) {
    setError("transfer too large");
    resetTransfer(true);
    return;
  }

  if (uploadFile_.write(payload, payloadSize) != payloadSize) {
    setError("transfer write failed");
    resetTransfer(true);
    return;
  }

  mbedtls_sha256_update(&shaContext_, payload, payloadSize);
  receivedBytes_ += payloadSize;
  expectedSequence_++;
  if (receivedBytes_ == expectedSize_ ||
      receivedBytes_ - lastProgressStatusBytes_ >= BLE_PROGRESS_STATUS_INTERVAL_BYTES) {
    lastProgressStatusBytes_ = receivedBytes_;
    statusDirty_ = true;
    requestUpdate();
  }
}

void BleTransferActivity::processCommit() {
  if (!transferOpen_) return;
  setState(State::VERIFYING);

  uploadFile_.flush();
  uploadFile_.close();
  transferOpen_ = false;

  if (receivedBytes_ != expectedSize_) {
    setError("size mismatch");
    resetTransfer(true);
    return;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaContext_, digest);
  shaActive_ = false;
  const std::string actualSha256 = sha256ToHex(digest);
  if (actualSha256 != expectedSha256_) {
    setError("sha256 mismatch");
    resetTransfer(true);
    return;
  }

  if (transferKind_ == TransferKind::BOOK && Storage.exists(finalPath_.c_str())) {
    setError("exists");
    resetTransfer(true);
    return;
  }
  if (transferKind_ == TransferKind::PACKAGE) {
    if (Storage.exists(finalPath_.c_str()) && !Storage.remove(finalPath_.c_str())) {
      setError("could not replace existing package");
      resetTransfer(true);
      return;
    }
  }
  if (!Storage.rename(partPath_.c_str(), finalPath_.c_str())) {
    setError("could not finalize transfer");
    resetTransfer(true);
    return;
  }
  removePartOnExit_ = false;

  if (transferKind_ == TransferKind::BOOK) {
    savedPath_ = finalPath_;
    completeFinalState(State::SAVED);
    return;
  }

  const auto archive = Marginalia::inspectPackageArchive(finalPath_);
  if (!archive.ok) {
    setError(archive.error.empty() ? "invalid package archive" : archive.error);
    return;
  }

  packageId_ = archive.packageId;
  packageName_ = archive.packageName;
  setState(State::INSTALLING);
  publishStatus();

  const auto extract = Marginalia::extractPackageArchiveToInbox(finalPath_);
  if (!extract.ok) {
    setError(extract.error);
    return;
  }

  const auto install = Marginalia::installInboxPackage(extract.packageId);
  if (!install.ok) {
    if (!Marginalia::removeInboxPackage(extract.packageId)) {
      LOG_ERR("BLE", "Failed to remove BLE inbox package after install error: %s", extract.packageId.c_str());
    }
    setError(install.error);
    return;
  }

  packageId_ = install.packageId;
  packageName_ = install.packageName;
  completeFinalState(State::INSTALLED);
}

void BleTransferActivity::startCrashReportDownload() {
  if (!Storage.exists(CRASH_REPORT_PATH)) {
    setError("not_found");
    return;
  }

  if (!Storage.openFileForRead("BLE", CRASH_REPORT_PATH, downloadFile_)) {
    setError("could not open crash report");
    return;
  }

  fileName_ = CRASH_REPORT_NAME;
  transferKind_ = TransferKind::CRASH_REPORT;
  expectedSize_ = downloadFile_.fileSize();
  sentBytes_ = 0;
  downloadSequence_ = 0;
  pendingDownloadAck_ = 0;
  downloadAwaitingAck_ = false;
  downloadOpen_ = true;
  setState(State::SENDING);
}

void BleTransferActivity::pumpDownload() {
  if (!downloadOpen_) return;
  if (downloadAwaitingAck_) {
    if (statusDirty_) publishStatus();
    return;
  }

  std::array<uint8_t, sizeof(uint32_t) + BLE_DOWNLOAD_CHUNK_BYTES> frame = {};
  frame[0] = static_cast<uint8_t>(downloadSequence_ & 0xFF);
  frame[1] = static_cast<uint8_t>((downloadSequence_ >> 8) & 0xFF);
  frame[2] = static_cast<uint8_t>((downloadSequence_ >> 16) & 0xFF);
  frame[3] = static_cast<uint8_t>((downloadSequence_ >> 24) & 0xFF);

  const int read = downloadFile_.read(frame.data() + sizeof(uint32_t), BLE_DOWNLOAD_CHUNK_BYTES);
  if (read < 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setError("crash report read failed");
    return;
  }

  if (read == 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setState(State::SENT);
    return;
  }

  ble_->notifyData(frame.data(), sizeof(uint32_t) + static_cast<size_t>(read));
  sentBytes_ += static_cast<size_t>(read);
  pendingDownloadAck_ = downloadSequence_;
  downloadAwaitingAck_ = true;
  downloadSequence_++;
  if (sentBytes_ == expectedSize_ || sentBytes_ - lastProgressStatusBytes_ >= BLE_PROGRESS_STATUS_INTERVAL_BYTES) {
    lastProgressStatusBytes_ = sentBytes_;
    statusDirty_ = true;
    requestUpdate();
  }
}

void BleTransferActivity::completeFinalState(const State finalState) {
  hostPaired_ = false;
  hostPairSkipped_ = false;
  if (!trustedHelloAccepted_ && !candidateHostId_.empty() && !candidateHostSecret_.empty()) {
    pendingFinalState_ = finalState;
    promptSelection_ = 0;
    setState(State::SAVE_HOST_PROMPT);
    return;
  }
  setState(finalState);
}

void BleTransferActivity::handleSaveHostPrompt() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (promptSelection_ > 0) {
      promptSelection_--;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (promptSelection_ < 1) {
      promptSelection_++;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (promptSelection_ == 0) {
      RenderLock lock(*this);
      hostPaired_ = BLE_TRUSTED_HOSTS.addOrReplaceHost(
          BleTrustedHost{candidateHostId_, candidateHostName_, candidateHostSecret_});
      hostPairSkipped_ = !hostPaired_;
    } else {
      hostPairSkipped_ = true;
    }
    candidateHostId_.clear();
    candidateHostName_.clear();
    candidateHostSecret_.clear();
    setState(pendingFinalState_);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    hostPairSkipped_ = true;
    candidateHostId_.clear();
    candidateHostName_.clear();
    candidateHostSecret_.clear();
    setState(pendingFinalState_);
  }
}

void BleTransferActivity::handleForgetHostPrompt() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (promptSelection_ > 0) {
      promptSelection_--;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (promptSelection_ < 1) {
      promptSelection_++;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (promptSelection_ == 1) {
      RenderLock lock(*this);
      if (!BLE_TRUSTED_HOSTS.clearAll()) {
        setError("could not forget trusted host");
        return;
      }
    }
    setState(State::ADVERTISING);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setState(State::ADVERTISING);
  }
}

void BleTransferActivity::resetTransfer(const bool removePart) {
  if (shaActive_) {
    mbedtls_sha256_free(&shaContext_);
    mbedtls_sha256_init(&shaContext_);
    shaActive_ = false;
  }
  if (uploadFile_) {
    uploadFile_.close();
  }
  if (downloadFile_) {
    downloadFile_.close();
  }
  if (removePart && removePartOnExit_ && !partPath_.empty() && Storage.exists(partPath_.c_str())) {
    Storage.remove(partPath_.c_str());
  }

  fileName_.clear();
  partPath_.clear();
  finalPath_.clear();
  expectedSha256_.clear();
  packageId_.clear();
  packageName_.clear();
  savedPath_.clear();
  transferKind_ = TransferKind::NONE;
  pendingFinalState_ = State::CONNECTED;
  expectedSize_ = 0;
  receivedBytes_ = 0;
  sentBytes_ = 0;
  lastProgressStatusBytes_ = 0;
  expectedSequence_ = 0;
  downloadSequence_ = 0;
  pendingDownloadAck_ = 0;
  transferOpen_ = false;
  downloadOpen_ = false;
  downloadAwaitingAck_ = false;
  pendingCommit_ = false;
  removePartOnExit_ = false;
}

void BleTransferActivity::setState(const State state) {
  state_ = state;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::setError(const std::string& error) {
  errorMessage_ = error;
  state_ = State::ERROR;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::publishStatus() {
  statusDirty_ = false;
  if (ble_) ble_->publish(buildStatusJson());
}

std::string BleTransferActivity::buildStatusJson() const {
  JsonDocument doc;
  const std::string state = stateName(state_);
  doc["state"] = state.c_str();
  doc["device_id"] = deviceId_.c_str();
  doc["device_nonce"] = deviceNonce_.c_str();
  doc["has_trusted_host"] = BLE_TRUSTED_HOSTS.hasHosts();
  if (!trustedHostName_.empty()) doc["trusted_host"] = trustedHostName_.c_str();
  if (hostPaired_) doc["paired"] = true;
  if (hostPairSkipped_) doc["pairing"] = "skipped";
  if (expectedSize_ > 0 || state_ == State::SENDING || state_ == State::SENT) {
    const std::string kind = transferKindName(transferKind_);
    if (!kind.empty()) doc["kind"] = kind.c_str();
    if (state_ == State::SENDING || state_ == State::SENT) {
      doc["sent"] = sentBytes_;
    } else {
      doc["received"] = receivedBytes_;
    }
    doc["size"] = expectedSize_;
  }
  if (!packageId_.empty()) doc["package"] = packageId_.c_str();
  if (!packageName_.empty()) doc["name"] = packageName_.c_str();
  if (state_ == State::SAVED && !savedPath_.empty()) {
    doc["name"] = fileName_.c_str();
    doc["path"] = savedPath_.c_str();
  }
  if (state_ == State::SENT) {
    doc["name"] = fileName_.c_str();
  }
  if (state_ == State::ERROR && !errorMessage_.empty()) doc["error"] = errorMessage_.c_str();

  String output;
  serializeJson(doc, output);
  return output.c_str();
}

void BleTransferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));

  const int centerY = pageHeight / 2 - 30;
  std::string primary;
  std::string secondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::ADVERTISING:
      primary = tr(STR_BLE_TRANSFER_READY);
      secondary = std::string(tr(STR_BLE_TRANSFER_CODE)) + sessionCode_;
      break;
    case State::CONNECTED:
      primary = tr(STR_CONNECTED);
      secondary = std::string(tr(STR_BLE_TRANSFER_CODE)) + sessionCode_;
      break;
    case State::RECEIVING: {
      primary = tr(STR_BLE_TRANSFER_RECEIVING);
      char buffer[48];
      snprintf(buffer, sizeof(buffer), "%u / %u bytes", static_cast<unsigned>(receivedBytes_),
               static_cast<unsigned>(expectedSize_));
      secondary = buffer;
      break;
    }
    case State::VERIFYING:
      primary = tr(STR_BLE_TRANSFER_VERIFYING);
      secondary = fileName_;
      break;
    case State::INSTALLING:
      primary = tr(STR_INSTALLING_PACKAGE);
      secondary = packageName_.empty() ? fileName_ : packageName_;
      break;
    case State::INSTALLED:
      primary = tr(STR_BLE_TRANSFER_INSTALLED);
      secondary = packageName_.empty() ? packageId_ : packageName_;
      break;
    case State::SAVED:
      primary = tr(STR_BLE_TRANSFER_SAVED);
      secondary = savedPath_.empty() ? fileName_ : savedPath_;
      break;
    case State::SENDING: {
      primary = "Sending diagnostic";
      char buffer[48];
      snprintf(buffer, sizeof(buffer), "%u / %u bytes", static_cast<unsigned>(sentBytes_),
               static_cast<unsigned>(expectedSize_));
      secondary = buffer;
      break;
    }
    case State::SENT:
      primary = "Diagnostic sent";
      secondary = fileName_;
      break;
    case State::SAVE_HOST_PROMPT:
      renderSaveHostPrompt();
      return;
    case State::FORGET_HOST_PROMPT:
      renderForgetHostPrompt();
      return;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      secondary = errorMessage_;
      break;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!secondary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8, secondary.c_str());
  }

  const char* forgetLabel = BLE_TRUSTED_HOSTS.hasHosts() && (state_ == State::ADVERTISING || state_ == State::CONNECTED)
                                ? tr(STR_FORGET_BUTTON)
                                : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", forgetLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::renderSaveHostPrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_BLE_SAVE_HOST), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top,
                            candidateHostName_.empty() ? "Trusted host" : candidateHostName_.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_BLE_SAVE_HOST_PROMPT));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  if (promptSelection_ == 0) {
    const std::string text = "[" + std::string(tr(STR_YES)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_YES));
  }

  if (promptSelection_ == 1) {
    const std::string text = "[" + std::string(tr(STR_NO)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_NO));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::renderForgetHostPrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_BLE_FORGET_HOST), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BLE_FORGET_HOST_PROMPT));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  if (promptSelection_ == 0) {
    const std::string text = "[" + std::string(tr(STR_CANCEL)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_CANCEL));
  }

  if (promptSelection_ == 1) {
    const std::string text = "[" + std::string(tr(STR_FORGET_BUTTON)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_FORGET_BUTTON));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
