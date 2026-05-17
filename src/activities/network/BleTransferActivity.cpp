#include "BleTransferActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

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
constexpr size_t MAX_BLE_PACKAGE_BYTES = 4UL * 1024UL * 1024UL;
constexpr size_t MAX_FILENAME_BYTES = 96;
constexpr size_t MPKG_SUFFIX_LEN = 9;

std::string makeSessionCode() {
  char buffer[7];
  snprintf(buffer, sizeof(buffer), "%06u", static_cast<unsigned>(esp_random() % 1000000UL));
  return buffer;
}

std::string toLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool isHexSha256(const std::string& value) {
  if (value.length() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool endsWithMpkgZip(const std::string& value) {
  constexpr const char* suffix = ".mpkg.zip";
  if (value.length() < MPKG_SUFFIX_LEN) return false;
  return toLowerAscii(value.substr(value.length() - MPKG_SUFFIX_LEN)) == suffix;
}

bool isSafeBlePackageName(const std::string& value) {
  if (value.empty() || value.length() > MAX_FILENAME_BYTES || value[0] == '.') return false;
  if (!endsWithMpkgZip(value)) return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

std::string sha256ToHex(const uint8_t digest[32]) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  return out;
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

  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { activity_.onBleConnected(); }

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

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override { activity_.onDataWrite(characteristic->getValue()); }

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
  ServerCallbacks serverCallbacks;
  ControlCallbacks controlCallbacks;
  DataCallbacks dataCallbacks;

  bool begin(BleTransferActivity& activity) {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks);

    service = server->createService(BLE_SERVICE_UUID);
    if (!service) return false;

    auto* control = service->createCharacteristic(BLE_CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    auto* dataIn = service->createCharacteristic(BLE_DATA_IN_UUID, NIMBLE_PROPERTY::WRITE_NR);
    status = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    if (!control || !dataIn || !status) return false;

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

  void end() {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    server = nullptr;
    service = nullptr;
    status = nullptr;
  }
};

BleTransferActivity::BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTransfer", renderer, mappedInput) {}

BleTransferActivity::~BleTransferActivity() = default;

void BleTransferActivity::onEnter() {
  Activity::onEnter();
  sessionCode_ = makeSessionCode();
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
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (pendingCommit_) {
    pendingCommit_ = false;
    processCommit();
    return;
  }

  if (statusDirty_) {
    publishStatus();
  }
}

void BleTransferActivity::onBleConnected() {
  helloAccepted_ = false;
  setState(State::CONNECTED);
}

void BleTransferActivity::onBleDisconnected() {
  if (transferOpen_) {
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
    if (version != 1) {
      setError("unsupported protocol version");
      return;
    }
    if (code != sessionCode_) {
      setError("invalid session code");
      return;
    }
    helloAccepted_ = true;
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

    if (kind != "package") {
      setError("unsupported transfer kind");
      return;
    }
    if (!isSafeBlePackageName(fileName_)) {
      setError("unsafe package filename");
      return;
    }
    if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_PACKAGE_BYTES) {
      setError("invalid package size");
      return;
    }
    if (!isHexSha256(expectedSha256_)) {
      setError("invalid sha256");
      return;
    }
    if (!Marginalia::ensurePackageBaseDirectories()) {
      setError("could not create package directories");
      return;
    }

    partPath_ = std::string(Marginalia::PACKAGE_SIDELOAD_ROOT) + "/.ble-" + fileName_ + ".part";
    finalPath_ = std::string(Marginalia::PACKAGE_SIDELOAD_ROOT) + "/" + fileName_;
    if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
    if (!Storage.openFileForWrite("BLE", partPath_, uploadFile_)) {
      setError("could not open package file");
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
    setError("package too large");
    resetTransfer(true);
    return;
  }

  if (uploadFile_.write(payload, payloadSize) != payloadSize) {
    setError("package write failed");
    resetTransfer(true);
    return;
  }

  mbedtls_sha256_update(&shaContext_, payload, payloadSize);
  receivedBytes_ += payloadSize;
  expectedSequence_++;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::processCommit() {
  if (!transferOpen_) return;
  setState(State::VERIFYING);

  uploadFile_.flush();
  uploadFile_.close();
  transferOpen_ = false;

  if (receivedBytes_ != expectedSize_) {
    setError("package size mismatch");
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

  if (Storage.exists(finalPath_.c_str()) && !Storage.remove(finalPath_.c_str())) {
    setError("could not replace existing package");
    resetTransfer(true);
    return;
  }
  if (!Storage.rename(partPath_.c_str(), finalPath_.c_str())) {
    setError("could not finalize package");
    resetTransfer(true);
    return;
  }
  removePartOnExit_ = false;

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
  setState(State::INSTALLED);
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
  if (removePart && removePartOnExit_ && !partPath_.empty() && Storage.exists(partPath_.c_str())) {
    Storage.remove(partPath_.c_str());
  }

  fileName_.clear();
  partPath_.clear();
  finalPath_.clear();
  expectedSha256_.clear();
  expectedSize_ = 0;
  receivedBytes_ = 0;
  expectedSequence_ = 0;
  transferOpen_ = false;
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
  doc["code"] = sessionCode_.c_str();
  if (expectedSize_ > 0) {
    doc["received"] = receivedBytes_;
    doc["size"] = expectedSize_;
  }
  if (!packageId_.empty()) doc["package"] = packageId_.c_str();
  if (!packageName_.empty()) doc["name"] = packageName_.c_str();
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
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      secondary = errorMessage_;
      break;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!secondary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8, secondary.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
