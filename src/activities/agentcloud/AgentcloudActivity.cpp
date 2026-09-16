#include "AgentcloudActivity.h"

#ifdef AGENTCLOUD_DASHBOARD

#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "AgentcloudAuth.h"
#include "AgentcloudMaterialIcons.h"
#include "fontIds.h"

namespace {

constexpr char DEVICE_NAME[] = "AC-X4";
constexpr char SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char WRITE_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr uint16_t MAX_BLE_WRITE_BYTES = 20;
constexpr uint32_t ADVERTISING_WATCHDOG_MS = 3000;
constexpr uint32_t CALLBACK_DRAIN_DELAY_MS = 10;
constexpr uint32_t CALLBACK_DRAIN_WARNING_MS = 5000;
constexpr int MESSAGE_TITLE_Y = 318;
constexpr int MESSAGE_BODY_Y = 386;
// The retained SSD1677 driver builds a temporary vector for each partial
// window. Preserve room for NimBLE and the render task instead of risking an
// abort from std::vector's throwing allocator in this no-exceptions build.
constexpr size_t WINDOW_HEAP_RESERVE = 16 * 1024;

const char* nextUtf8Codepoint(const char* cursor) {
  const uint8_t lead = static_cast<uint8_t>(*cursor);
  size_t bytes = 1;
  if ((lead & 0xe0) == 0xc0) {
    bytes = 2;
  } else if ((lead & 0xf0) == 0xe0) {
    bytes = 3;
  } else if ((lead & 0xf8) == 0xf0) {
    bytes = 4;
  }
  for (size_t i = 1; i < bytes; ++i) {
    if (cursor[i] == '\0' || (static_cast<uint8_t>(cursor[i]) & 0xc0) != 0x80) return cursor + 1;
  }
  return cursor + bytes;
}

size_t trimLastUtf8Codepoint(const char* text, size_t length) {
  if (length == 0) return 0;
  --length;
  while (length != 0 && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80) --length;
  return length;
}

const char* frameResultName(const agentcloud::FrameResult result) {
  switch (result) {
    case agentcloud::FrameResult::InvalidLength:
      return "length";
    case agentcloud::FrameResult::InvalidType:
      return "type";
    case agentcloud::FrameResult::InvalidAuth:
      return "auth";
    case agentcloud::FrameResult::WrongConnection:
      return "connection";
    case agentcloud::FrameResult::OutOfSequence:
      return "sequence";
    case agentcloud::FrameResult::Overflow:
      return "overflow";
    default:
      return "none";
  }
}

const char* parseErrorName(const agentcloud::ParseError error) {
  switch (error) {
    case agentcloud::ParseError::Empty:
      return "empty";
    case agentcloud::ParseError::TooLong:
      return "length";
    case agentcloud::ParseError::NonPrintable:
      return "character";
    case agentcloud::ParseError::InvalidJson:
      return "json";
    case agentcloud::ParseError::InvalidShape:
      return "shape";
    case agentcloud::ParseError::TooManyRows:
      return "rows";
    case agentcloud::ParseError::MissingRows:
      return "row-count";
    case agentcloud::ParseError::MissingId:
      return "id";
    case agentcloud::ParseError::TextTooLong:
      return "text-length";
    case agentcloud::ParseError::TooDeep:
      return "depth";
    default:
      return "none";
  }
}

}  // namespace

class AgentcloudBle final {
 public:
  static AgentcloudBle& instance() {
    static AgentcloudBle singleton;
    return singleton;
  }

  bool begin(AgentcloudActivity& activity) {
    AgentcloudActivity* expected = nullptr;
    if (!owner.compare_exchange_strong(expected, &activity)) {
      LOG_ERR("ACBLE", "BLE lifecycle is already bound");
      return false;
    }

    ending.store(false);
    if (NimBLEDevice::isInitialized()) return reuseInitialized(activity);
    if (objectsReady || server != nullptr || writeCharacteristic != nullptr || advertising != nullptr) {
      return failBegin(activity, "retained BLE objects are unavailable");
    }

    if (!NimBLEDevice::init(DEVICE_NAME)) {
      return failBegin(activity, "NimBLE initialization failed");
    }
    NimBLEDevice::setMTU(23);

    server = NimBLEDevice::createServer();
    if (server == nullptr) return failBegin(activity, "server allocation failed");
    server->setCallbacks(&serverCallbacks, false);

    NimBLEService* service = server->createService(SERVICE_UUID);
    if (service == nullptr) return failBegin(activity, "service allocation failed");
    writeCharacteristic = service->createCharacteristic(WRITE_UUID, NIMBLE_PROPERTY::WRITE, MAX_BLE_WRITE_BYTES);
    if (writeCharacteristic == nullptr) return failBegin(activity, "characteristic allocation failed");
    writeCharacteristic->setCallbacks(&characteristicCallbacks);
    if (!service->start()) return failBegin(activity, "service start failed");

    advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr) return failBegin(activity, "advertising allocation failed");
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setName(DEVICE_NAME);
    objectsReady = true;
    if (!startAdvertising()) return failBegin(activity, "initial advertising failed");
    return true;
  }

  void detachAndStop(AgentcloudActivity& activity) {
    ending.store(true);
    AgentcloudActivity* detachedOwner = owner.exchange(nullptr);
    if (detachedOwner != nullptr && detachedOwner != &activity) {
      LOG_ERR("ACBLE", "BLE lifecycle owner mismatch");
    }
    waitForCallbacks();

    advertisingActive.store(false);
    if (advertising != nullptr && advertising->isAdvertising() && !advertising->stop()) {
      LOG_ERR("ACBLE", "Advertising stop failed");
    }
    if (server != nullptr && connected.load()) {
      if (!server->disconnect(connectionHandle.load())) LOG_ERR("ACBLE", "Disconnect request failed");
      for (uint8_t attempt = 0; attempt < 60 && connected.load(); ++attempt) vTaskDelay(pdMS_TO_TICKS(10));
      if (connected.load()) LOG_ERR("ACBLE", "Disconnect acknowledgement timed out");
    }

    if (!NimBLEDevice::deinit(false)) {
      LOG_ERR("ACBLE", "NimBLE stop failed; retaining callback objects detached");
      return;
    }
    if (!NimBLEDevice::deinit(true)) {
      LOG_ERR("ACBLE", "NimBLE object cleanup failed; retaining detached pointers");
      return;
    }

    objectsReady = false;
    connected.store(false);
    connectionHandle.store(agentcloud::NO_CONNECTION);
    server = nullptr;
    writeCharacteristic = nullptr;
    advertising = nullptr;
  }

  bool maintainAdvertising() {
    if (!NimBLEDevice::isInitialized() || owner.load() == nullptr || ending.load() || connected.load()) return true;
    if (advertising != nullptr && advertising->isAdvertising()) {
      advertisingActive.store(true);
      return true;
    }
    advertisingActive.store(false);
    return startAdvertising();
  }

 private:
  class CallbackScope final {
   public:
    explicit CallbackScope(AgentcloudBle& parent) : parent(parent) { parent.activeCallbacks.fetch_add(1); }
    ~CallbackScope() { parent.activeCallbacks.fetch_sub(1); }

   private:
    AgentcloudBle& parent;
  };

  class ServerCallbacks final : public NimBLEServerCallbacks {
   public:
    explicit ServerCallbacks(AgentcloudBle& parent) : parent(parent) {}

    void onConnect(NimBLEServer* callbackServer, NimBLEConnInfo& connectionInfo) override {
      CallbackScope callback(parent);
      const uint16_t handle = connectionInfo.getConnHandle();
      AgentcloudActivity* callbackOwner = parent.owner.load();
      if (callbackOwner == nullptr || callbackOwner->stopping.load()) {
        callbackServer->disconnect(handle);
        return;
      }
      bool expected = false;
      if (parent.ending.load() || !parent.connected.compare_exchange_strong(expected, true)) {
        callbackServer->disconnect(handle);
        return;
      }
      parent.connectionHandle.store(handle);
      parent.advertisingActive.store(false);
      callbackOwner->connectionOpened(handle);
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& connectionInfo, int) override {
      CallbackScope callback(parent);
      AgentcloudActivity* callbackOwner = parent.owner.load();
      const uint16_t handle = connectionInfo.getConnHandle();
      if (handle != parent.connectionHandle.load()) return;
      parent.connected.store(false);
      parent.connectionHandle.store(agentcloud::NO_CONNECTION);
      parent.advertisingActive.store(false);
      if (callbackOwner == nullptr || callbackOwner->stopping.load()) return;
      callbackOwner->connectionClosed(handle);
      if (!parent.ending.load()) parent.startAdvertising();
    }

   private:
    AgentcloudBle& parent;
  };

  class CharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
   public:
    explicit CharacteristicCallbacks(AgentcloudBle& parent) : parent(parent) {}

    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connectionInfo) override {
      CallbackScope callback(parent);
      AgentcloudActivity* callbackOwner = parent.owner.load();
      if (callbackOwner == nullptr || callbackOwner->stopping.load() || parent.ending.load()) return;
      const NimBLEAttValue& value = characteristic->getValue();
      callbackOwner->receiveFrame(connectionInfo.getConnHandle(), value.data(), value.length());
    }

   private:
    AgentcloudBle& parent;
  };

  AgentcloudBle() : serverCallbacks(*this), characteristicCallbacks(*this) {}
  ~AgentcloudBle() = default;

  bool reuseInitialized(AgentcloudActivity& activity) {
    if (!objectsReady || server == nullptr || writeCharacteristic == nullptr || advertising == nullptr) {
      return failBegin(activity, "initialized BLE stack cannot be safely reused");
    }
    if (connected.load() || server->getConnectedCount() != 0) {
      return failBegin(activity, "retained BLE connection is still active");
    }
    server->setCallbacks(&serverCallbacks, false);
    writeCharacteristic->setCallbacks(&characteristicCallbacks);
    if (advertising->isAdvertising()) {
      advertisingActive.store(true);
      return true;
    }
    if (!startAdvertising()) return failBegin(activity, "retained advertising restart failed");
    return true;
  }

  bool startAdvertising() {
    if (advertising == nullptr || ending.load() || connected.load()) return false;
    bool expected = false;
    if (!advertisingActive.compare_exchange_strong(expected, true)) return true;
    const bool started = advertising->start();
    if (!started) {
      advertisingActive.store(false);
      LOG_ERR("ACBLE", "Advertising start failed");
    }
    return started;
  }

  bool failBegin(AgentcloudActivity& activity, const char* reason) {
    LOG_ERR("ACBLE", "%s", reason);
    detachAndStop(activity);
    return false;
  }

  void waitForCallbacks() {
    uint32_t waitedMs = 0;
    bool warned = false;
    while (activeCallbacks.load() != 0) {
      if (!warned && waitedMs >= CALLBACK_DRAIN_WARNING_MS) {
        LOG_ERR("ACBLE", "Still draining callbacks: %lu active",
                static_cast<unsigned long>(activeCallbacks.load()));
        warned = true;
      }
      vTaskDelay(pdMS_TO_TICKS(CALLBACK_DRAIN_DELAY_MS));
      waitedMs += CALLBACK_DRAIN_DELAY_MS;
    }
  }

  ServerCallbacks serverCallbacks;
  CharacteristicCallbacks characteristicCallbacks;
  std::atomic<AgentcloudActivity*> owner{nullptr};
  std::atomic<uint32_t> activeCallbacks{0};
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* writeCharacteristic = nullptr;
  NimBLEAdvertising* advertising = nullptr;
  std::atomic<uint16_t> connectionHandle{agentcloud::NO_CONNECTION};
  std::atomic<bool> connected{false};
  std::atomic<bool> advertisingActive{false};
  std::atomic<bool> ending{true};
  bool objectsReady = false;
};

AgentcloudActivity::AgentcloudActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Agentcloud", renderer, mappedInput) {}

AgentcloudActivity::~AgentcloudActivity() = default;

static_assert(sizeof(AgentcloudActivity) < 7000, "Agentcloud activity state exceeded its RAM budget");

void AgentcloudActivity::logInternalHeap(const char* stage) const {
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("ACD", "%s: free=%zu largest=%zu", stage, heap.freeBytes, heap.largestBlockBytes);
}

void AgentcloudActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  stopping.store(false);
  screenState = ScreenState::Waiting;
  firstPaint = true;
  dirtyRows = 0;
  partialRefreshCount = 0;
  spinnerPhase = 0;
  spinnerOnly = false;
  forceFullRefresh = false;
  lastSpinnerStepMs = millis();
  lastAdvertisingCheckMs = lastSpinnerStepMs;

  logInternalHeap("heap before BLE preparation");
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  logInternalHeap("heap before BLE initialization");

  if (!agentcloud::hasValidAuthenticator()) {
    LOG_ERR("ACD", "Agentcloud authenticator is not configured; BLE disabled");
    showError(ScreenState::AuthError);
    return;
  }

  // One queue slot is the only payload-sized heap allocation: it crosses the
  // NimBLE-host/main-task boundary atomically and intentionally drops stale work.
  payloadQueue = xQueueCreate(1, sizeof(agentcloud::PayloadMessage));
  if (payloadQueue == nullptr) {
    LOG_ERR("ACD", "OOM: payload queue (%zu-byte item)", sizeof(agentcloud::PayloadMessage));
    showError(ScreenState::BleError);
    return;
  }

  // The BLE singleton and its callbacks have process lifetime. This activity
  // holds only a non-owning pointer while it is bound as the callback owner.
  ble = &AgentcloudBle::instance();
  if (!ble->begin(*this)) {
    ble = nullptr;
    showError(ScreenState::BleError);
    return;
  }

  logInternalHeap("heap after BLE initialization");
  requestUpdateAndWait();
  LOG_INF("ACD", "Dashboard ready; advertising as %s", DEVICE_NAME);
  logInternalHeap("heap at dashboard ready");
}

void AgentcloudActivity::onExit() {
  stopping.store(true);
  stopBle();
  if (payloadQueue != nullptr) {
    vQueueDelete(payloadQueue);
    payloadQueue = nullptr;
  }
  frameAssembler.reset();
  Activity::onExit();
}

void AgentcloudActivity::stopBle() {
  if (ble != nullptr) {
    ble->detachAndStop(*this);
    ble = nullptr;
  }
}

void AgentcloudActivity::showError(const ScreenState errorState) {
  screenState = errorState;
  requestUpdateAndWait();
}

void AgentcloudActivity::connectionOpened(const uint16_t connectionHandle) {
  frameAssembler.connect(connectionHandle);
  LOG_INF("ACD", "BLE client connected");
}

void AgentcloudActivity::connectionClosed(const uint16_t connectionHandle) {
  frameAssembler.disconnect(connectionHandle);
  LOG_INF("ACD", "BLE client disconnected");
}

void AgentcloudActivity::receiveFrame(const uint16_t connectionHandle, const uint8_t* bytes, const size_t length) {
  if (stopping.load() || payloadQueue == nullptr) return;
  const agentcloud::FrameResult result =
      frameAssembler.accept(connectionHandle, bytes, length, agentcloud::AUTHENTICATOR);
  if (result == agentcloud::FrameResult::Complete) {
    xQueueOverwrite(payloadQueue, &frameAssembler.message());
    return;
  }
  if (result != agentcloud::FrameResult::Accepted) {
    LOG_ERR("ACD", "Rejected BLE frame: %s", frameResultName(result));
  }
}

void AgentcloudActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    activityManager.goHome();
    return;
  }

  if (payloadQueue != nullptr && xQueueReceive(payloadQueue, &queuedPayload, 0) == pdTRUE) {
    agentcloud::ParseError error = agentcloud::ParseError::None;
    if (!agentcloud::parsePayload(queuedPayload.bytes, queuedPayload.length, parsedDashboard, error)) {
      LOG_ERR("ACD", "Rejected dashboard payload: %s", parseErrorName(error));
    } else {
      uint8_t changed = agentcloud::dirtyRowMask(dashboard, parsedDashboard);
      if (changed == 0 && screenState == ScreenState::Dashboard) {
        LOG_DBG("ACD", "Duplicate dashboard payload ignored");
      } else {
        const uint32_t now = millis();
        const uint8_t previousActiveRows = agentcloud::activeRowMask(dashboard);
        const uint8_t incomingActiveRows = agentcloud::activeRowMask(parsedDashboard);
        if (agentcloud::hasNewSettledUnreadIdentity(dashboard, parsedDashboard)) forceFullRefresh = true;
        if (screenState == ScreenState::Dashboard &&
            agentcloud::allRowsEmpty(dashboard) != agentcloud::allRowsEmpty(parsedDashboard)) {
          // The centred all-clear composition overlaps row boundaries, so a
          // transition into or out of it necessarily invalidates the panel.
          changed = 0x0f;
        }
        dashboard = parsedDashboard;
        dirtyRows = screenState == ScreenState::Dashboard
                        ? agentcloud::applyContentSpinnerStep(changed, previousActiveRows, incomingActiveRows, now,
                                                             spinnerPhase, lastSpinnerStepMs)
                        : 0x0f;
        spinnerOnly = false;
        screenState = ScreenState::Dashboard;
        if (previousActiveRows == 0 && incomingActiveRows != 0) {
          spinnerPhase = 0;
          lastSpinnerStepMs = now;
        }
        requestUpdateAndWait();
      }
    }
  }

  const uint32_t now = millis();
  const uint8_t activeRows = agentcloud::activeRowMask(dashboard);
  if (screenState == ScreenState::Dashboard && agentcloud::spinnerDue(lastSpinnerStepMs, now, activeRows != 0)) {
    spinnerPhase = static_cast<uint8_t>((spinnerPhase + 1) & 7);
    dirtyRows = activeRows;
    spinnerOnly = true;
    lastSpinnerStepMs = now;
    requestUpdateAndWait();
  }

  if (ble && static_cast<uint32_t>(now - lastAdvertisingCheckMs) >= ADVERTISING_WATCHDOG_MS) {
    lastAdvertisingCheckMs = now;
    ble->maintainAdvertising();
  }
}

void AgentcloudActivity::renderMessage(const char* message) {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, MESSAGE_TITLE_Y, tr(STR_AGENTCLOUD_TITLE), true,
                            EpdFontFamily::REGULAR);
  drawWrappedText(NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR, agentcloud::ROW_PADDING, MESSAGE_BODY_Y,
                  width - agentcloud::ROW_PADDING * 2, agentcloud::BODY_LINE_HEIGHT, 3, message, true, true);
}

uint8_t AgentcloudActivity::drawWrappedText(const int fontId, const EpdFontFamily::Style style, const int x,
                                            const int y, const int width, const int lineStep, const uint8_t maxLines,
                                            const char* text, const bool ink, const bool centered) {
  if (text == nullptr || *text == '\0' || width <= 0 || maxLines == 0) return 0;

  constexpr char ELLIPSIS[] = "\xe2\x80\xa6";
  const char* cursor = text;
  uint8_t lines = 0;
  while (*cursor != '\0' && lines < maxLines) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') break;

    const char* const lineStart = cursor;
    const char* scan = cursor;
    const char* fitEnd = cursor;
    const char* lastSpace = nullptr;
    while (*scan != '\0') {
      const char* const next = nextUtf8Codepoint(scan);
      const size_t candidateLength = static_cast<size_t>(next - lineStart);
      if (candidateLength >= sizeof(textLineScratch)) break;
      memcpy(textLineScratch, lineStart, candidateLength);
      textLineScratch[candidateLength] = '\0';
      if (renderer.getTextWidth(fontId, textLineScratch, style) > width) break;
      fitEnd = next;
      if (*scan == ' ') lastSpace = scan;
      scan = next;
    }

    if (fitEnd == lineStart) fitEnd = nextUtf8Codepoint(lineStart);
    const bool widthOverflow = *fitEnd != '\0';
    const char* lineEnd = fitEnd;
    const char* nextCursor = fitEnd;
    if (widthOverflow && lastSpace != nullptr && lastSpace > lineStart) {
      lineEnd = lastSpace;
      nextCursor = lastSpace + 1;
    }
    while (lineEnd > lineStart && lineEnd[-1] == ' ') --lineEnd;
    while (*nextCursor == ' ') ++nextCursor;

    size_t lineLength = static_cast<size_t>(lineEnd - lineStart);
    memcpy(textLineScratch, lineStart, lineLength);
    textLineScratch[lineLength] = '\0';
    if (lines + 1 == maxLines && *nextCursor != '\0') {
      while (true) {
        memcpy(textLineScratch + lineLength, ELLIPSIS, sizeof(ELLIPSIS));
        if (renderer.getTextWidth(fontId, textLineScratch, style) <= width || lineLength == 0) break;
        lineLength = trimLastUtf8Codepoint(textLineScratch, lineLength);
      }
    }

    int drawX = x;
    if (centered) drawX += std::max(0, (width - renderer.getTextWidth(fontId, textLineScratch, style)) / 2);
    renderer.drawText(fontId, drawX, y + lines * lineStep, textLineScratch, ink, style);
    ++lines;
    cursor = nextCursor;
  }
  return lines;
}

void AgentcloudActivity::drawStateIcon(const agentcloud::CardState state, const int x, const int y,
                                       const bool ink) const {
  const uint8_t* mask = nullptr;
  switch (state) {
    case agentcloud::CardState::Unread:
      mask = state_icon_unread_bits;
      break;
    case agentcloud::CardState::Read:
      mask = state_icon_read_bits;
      break;
    case agentcloud::CardState::Waiting:
      mask = state_icon_question_bits;
      break;
    case agentcloud::CardState::Working:
      mask = state_icon_working_frames[spinnerPhase & 7];
      break;
    case agentcloud::CardState::Empty:
      break;
  }
  if (mask == nullptr) return;
  constexpr int ROW_BYTES = (STATE_ICON_UNREAD_W + 7) / 8;
  for (int row = 0; row < STATE_ICON_UNREAD_H; ++row) {
    for (int column = 0; column < STATE_ICON_UNREAD_W; ++column) {
      if ((mask[row * ROW_BYTES + column / 8] & (0x80u >> (column & 7))) != 0) {
        renderer.drawPixel(x + column, y + row, ink);
      }
    }
  }
}

void AgentcloudActivity::renderRow(const size_t index, const agentcloud::Rect& rect) {
  const agentcloud::Card& card = dashboard.rows[index];
  const bool highlighted = agentcloud::isHighlighted(card);
  const bool ink = !highlighted;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, highlighted);
  const int separator = agentcloud::separatorY(index, rect);
  if (separator >= 0) renderer.drawLine(rect.x, separator, rect.x + rect.width - 1, separator, true);
  if (card.title[0] == '\0') return;

  const agentcloud::Rect icon = agentcloud::stateIconRect(rect);
  const int textX = rect.x + agentcloud::ROW_PADDING;
  const int titleWidth = std::max(0, icon.x - agentcloud::STATE_ICON_GAP - textX);
  const int bodyWidth = std::max(0, rect.width - agentcloud::ROW_PADDING * 2);
  renderer.setClipRect(rect.x + agentcloud::ROW_PADDING, rect.y + agentcloud::ROW_PADDING, bodyWidth,
                       rect.height - agentcloud::ROW_PADDING * 2);
  const uint8_t titleLines = drawWrappedText(NOTOSANS_14_FONT_ID, EpdFontFamily::BOLD, textX,
                                             rect.y + agentcloud::ROW_PADDING, titleWidth,
                                             agentcloud::TITLE_LINE_HEIGHT, agentcloud::TITLE_MAX_LINES, card.title, ink);
  drawWrappedText(NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR, textX,
                  rect.y + agentcloud::ROW_PADDING + titleLines * agentcloud::TITLE_LINE_HEIGHT +
                      agentcloud::TITLE_BODY_GAP,
                  bodyWidth, agentcloud::BODY_LINE_HEIGHT, agentcloud::bodyLineBudget(titleLines), card.headline, ink);
  renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  drawStateIcon(agentcloud::cardState(card), icon.x, icon.y, ink);
}

void AgentcloudActivity::renderStateOnly(const size_t index, const agentcloud::Rect& rowRect) {
  const agentcloud::Card& card = dashboard.rows[index];
  const bool highlighted = agentcloud::isHighlighted(card);
  const agentcloud::Rect icon = agentcloud::stateIconRect(rowRect);
  renderer.fillRect(icon.x, icon.y, icon.width, icon.height, highlighted);
  drawStateIcon(agentcloud::cardState(card), icon.x, icon.y, !highlighted);
}

void AgentcloudActivity::render(RenderLock&&) {
  if (screenState != ScreenState::Dashboard) {
    const char* message = tr(STR_AGENTCLOUD_WAITING_FOR_MAC);
    if (screenState == ScreenState::AuthError) message = tr(STR_AGENTCLOUD_AUTH_ERROR);
    if (screenState == ScreenState::BleError) message = tr(STR_AGENTCLOUD_BLE_ERROR);
    renderMessage(message);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    partialRefreshCount = 0;
    return;
  }

  const agentcloud::DashboardLayout layout =
      agentcloud::makeLayout(renderer.getScreenWidth(), renderer.getScreenHeight());
  uint8_t rowsToDraw = firstPaint ? 0x0f : dirtyRows;
  agentcloud::Rect updateBounds{};
  if (agentcloud::allRowsEmpty(dashboard)) {
    renderer.clearScreen();
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, MESSAGE_TITLE_Y, tr(STR_AGENTCLOUD_ALL_CLEAR), true,
                              EpdFontFamily::REGULAR);
    drawWrappedText(NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR, agentcloud::ROW_PADDING, MESSAGE_BODY_Y,
                    renderer.getScreenWidth() - agentcloud::ROW_PADDING * 2, agentcloud::BODY_LINE_HEIGHT, 2,
                    tr(STR_AGENTCLOUD_WAITING_FOR_ACTIVITY), true, true);
    updateBounds = {0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  } else {
    for (size_t i = 0; i < agentcloud::ROW_COUNT; ++i) {
      if ((rowsToDraw & (1u << i)) == 0) continue;
      if (spinnerOnly && !firstPaint) {
        renderStateOnly(i, layout.rows[i]);
        const agentcloud::Rect icon = agentcloud::stateIconRect(layout.rows[i]);
        if (updateBounds.width == 0) {
          updateBounds = icon;
        } else {
          const int right = std::max(updateBounds.x + updateBounds.width, icon.x + icon.width);
          const int bottom = std::max(updateBounds.y + updateBounds.height, icon.y + icon.height);
          updateBounds.x = std::min(updateBounds.x, icon.x);
          updateBounds.y = std::min(updateBounds.y, icon.y);
          updateBounds.width = right - updateBounds.x;
          updateBounds.height = bottom - updateBounds.y;
        }
      } else {
        renderRow(i, layout.rows[i]);
      }
    }
    if (!spinnerOnly || firstPaint) updateBounds = agentcloud::dirtyBounds(layout, rowsToDraw);
  }

  if (forceFullRefresh) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    partialRefreshCount = 0;
    forceFullRefresh = false;
  } else if (firstPaint || agentcloud::partialCleanupDue(partialRefreshCount)) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    partialRefreshCount = 0;
  } else if (updateBounds.width > 0 && updateBounds.height > 0) {
    const size_t windowBytes =
        renderer.getRegionByteSize(updateBounds.x, updateBounds.y, updateBounds.width, updateBounds.height);
    const auto heap = HalMemory::getInternalHeap();
    const bool hasWindowHeadroom =
        windowBytes <= heap.largestBlockBytes && WINDOW_HEAP_RESERVE <= heap.largestBlockBytes - windowBytes;
    if (windowBytes != 0 && hasWindowHeadroom) {
      renderer.displayWindow(updateBounds.x, updateBounds.y, updateBounds.width, updateBounds.height);
      ++partialRefreshCount;
    } else {
      LOG_ERR("ACD", "Partial window unsafe: bytes=%zu largest=%zu; using half refresh", windowBytes,
              heap.largestBlockBytes);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      partialRefreshCount = 0;
    }
  }
  firstPaint = false;
  spinnerOnly = false;
  dirtyRows = 0;
}

#endif
