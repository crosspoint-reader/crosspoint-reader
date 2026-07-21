#include "NearbyStatsSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#ifndef SIMULATOR
#include <esp_mac.h>
#endif

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/NearbyStatsStorage.h"
#include "activities/reader/ReadingStatsCodec.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char LOG_TAG[] = "CVNSTATS";
constexpr char CROSSPOINT_ROOT[] = "/.crosspoint";
constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";

std::string exchangeErrorMessage(const NearbySyncExchange::Error error) {
  switch (error) {
    case NearbySyncExchange::Error::RadioStart:
      return tr(STR_NEARBY_ERROR_RADIO);
    case NearbySyncExchange::Error::NoPeer:
      return tr(STR_NEARBY_ERROR_NO_PEER);
    case NearbySyncExchange::Error::PairingTimeout:
      return tr(STR_NEARBY_ERROR_PAIR_TIMEOUT);
    case NearbySyncExchange::Error::TransferTimeout:
      return tr(STR_NEARBY_ERROR_TRANSFER_TIMEOUT);
    case NearbySyncExchange::Error::QueueOverflow:
    case NearbySyncExchange::Error::Protocol:
      return tr(STR_NEARBY_ERROR_PROTOCOL);
    case NearbySyncExchange::Error::None:
      return {};
  }
  return tr(STR_NEARBY_ERROR_PROTOCOL);
}

std::string statsPath(const NearbySync::MacAddress& mac) {
  char path[64];
  snprintf(path, sizeof(path), "%s/device_%02x%02x%02x%02x%02x%02x_v4.bin", SYNCED_STATS_DIR, mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return path;
}

std::string legacyStatsPath(const NearbySync::MacAddress& mac) {
  char path[64];
  snprintf(path, sizeof(path), "%s/device_%02x%02x%02x%02x%02x%02x.bin", SYNCED_STATS_DIR, mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return path;
}

std::string formatStatsSummary(const GlobalReadingStats& stats) {
  char value[64];
  snprintf(value, sizeof(value), tr(STR_NEARBY_STATS_FORMAT), static_cast<unsigned long>(stats.totalSessions),
           static_cast<unsigned long>(stats.totalPagesTurned));
  return value;
}

std::string formatStatsTime(const GlobalReadingStats& stats) {
  char value[64];
  const uint32_t hours = stats.totalReadingSeconds / 3600u;
  const uint32_t minutes = stats.totalReadingSeconds % 3600u / 60u;
  snprintf(value, sizeof(value), tr(STR_NEARBY_STATS_TIME_FORMAT), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes));
  return value;
}

std::string formatStatsHistory(const GlobalReadingStats& stats) {
  char value[80];
  snprintf(value, sizeof(value), tr(STR_NEARBY_STATS_HISTORY_FORMAT), static_cast<unsigned long>(stats.completedBooks),
           static_cast<unsigned>(stats.displayLongestReadingStreak()));
  return value;
}

std::string deviceName(const NearbySync::MacAddress& mac) {
  char value[24];
  snprintf(value, sizeof(value), "CrossVi-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return value;
}

std::string labeled(const char* label, const std::string& value) { return std::string(label) + ": " + value; }
}  // namespace

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput) {}

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
#ifdef SIMULATOR
  setError(tr(STR_NEARBY_ERROR_SIMULATOR));
#else
  if (esp_efuse_mac_get_default(localMac_.data()) != ESP_OK) setError(tr(STR_NEARBY_ERROR_DEVICE_ID));
#endif
  GlobalReadingStats::LoadStatus statsStatus = GlobalReadingStats::LoadStatus::Missing;
  localStats_ = GlobalReadingStats::load(&statsStatus);
  localStatsTrusted_ = GlobalReadingStats::isTrustedLoadStatus(statsStatus);
  if (localStatsTrusted_) localOffer_ = ReadingStatsCodec::encode(localStats_);
  requestUpdate();
}

void NearbyStatsSyncActivity::onExit() {
  const bool restartHome = exchange_.wasRadioActivated();
  exchange_.stop();
  Activity::onExit();
  if (restartHome) silentRestart();
}

void NearbyStatsSyncActivity::loop() {
  // render() runs on a separate task and reads the exchange plus strings and
  // decoded statistics below. Keep every mutation in one render-locked input
  // transaction so ESP-NOW transitions cannot race an e-paper refresh.
  RenderLock lock(*this);
  const uint32_t now = millis();
  exchange_.update(now);
  handleExchangeState();
  if (exchange_.readyToExit(now)) {
    exitActivity();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitActivity();
    return;
  }
  if (!errorMessage_.empty()) return;

  if (exchange_.state() == NearbySyncExchange::State::Idle) {
    const bool receive = mappedInput.wasPressed(MappedInputManager::Button::NavPrevious);
    const bool send = mappedInput.wasPressed(MappedInputManager::Button::NavNext);
    if (receive || send) startExchange(receive ? NearbySync::Role::Receiver : NearbySync::Role::Sender);
    return;
  }

  if (!mappedInput.wasPressed(MappedInputManager::Button::Confirm)) return;
  switch (exchange_.state()) {
    case NearbySyncExchange::State::Pairing:
      if (!exchange_.confirmPairing(now)) setError(tr(STR_NEARBY_ERROR_PROTOCOL));
      break;
    case NearbySyncExchange::State::OfferReady:
      if (!peerStatsRegresses_ && !peerStatsStorageProtected_) acceptPeerStats();
      break;
    default:
      break;
  }
  handleExchangeState();
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_NEARBY_STATS_SYNC));

  int y = screen.y + metrics.topPadding + metrics.headerHeight + 56;
  auto line = [&](const std::string& text, const bool bold = false) {
    if (text.empty()) return;
    const int font = bold ? UI_10_FONT_ID : SMALL_FONT_ID;
    const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string safe = renderer.truncatedText(font, text.c_str(), std::max(0, screen.width - 24), style);
    UITheme::drawCenteredText(renderer, screen, font, y, safe.c_str(), true, style);
    y += renderer.getLineHeight(font) + metrics.verticalSpacing;
  };

  std::string confirmLabel;
  std::string previousLabel;
  std::string nextLabel;
  if (!errorMessage_.empty()) {
    line(tr(STR_ERROR_MSG), true);
    line(errorMessage_);
  } else {
    switch (exchange_.state()) {
      case NearbySyncExchange::State::Idle:
        line(tr(STR_NEARBY_STATS_READY), true);
        line(localStatsTrusted_ ? formatStatsSummary(localStats_) : tr(STR_NEARBY_LOCAL_STATS_UNAVAILABLE));
        line(tr(STR_NEARBY_CHOOSE_DIRECTION));
        line(tr(STR_NEARBY_MANUAL_ONLY));
        line(tr(STR_NEARBY_TRUST_WARNING));
        previousLabel = tr(STR_NEARBY_RECEIVE);
        nextLabel = tr(STR_NEARBY_SEND);
        break;
      case NearbySyncExchange::State::Discovering:
        line(tr(STR_NEARBY_DISCOVERING), true);
        line(exchange_.role() == NearbySync::Role::Sender ? tr(STR_NEARBY_ROLE_SENDER) : tr(STR_NEARBY_ROLE_RECEIVER));
        line(tr(STR_NEARBY_CHOOSE_OPPOSITE));
        break;
      case NearbySyncExchange::State::Pairing: {
        line(tr(STR_NEARBY_VERIFY_CODE), true);
        const std::string peer = exchange_.peerName().empty() ? tr(STR_NEARBY_UNKNOWN_READER) : exchange_.peerName();
        line(exchange_.role() == NearbySync::Role::Sender ? tr(STR_NEARBY_ROLE_SENDER) : tr(STR_NEARBY_ROLE_RECEIVER));
        line(labeled(
            exchange_.role() == NearbySync::Role::Sender ? tr(STR_NEARBY_TO_DEVICE) : tr(STR_NEARBY_FROM_DEVICE),
            peer));
        char code[16];
        snprintf(code, sizeof(code), "%04u", static_cast<unsigned>(exchange_.pairingCode()));
        line(code, true);
        line(exchange_.role() == NearbySync::Role::Sender ? tr(STR_NEARBY_PAIR_SEND_HINT)
                                                          : tr(STR_NEARBY_PAIR_RECEIVE_HINT));
        confirmLabel = tr(STR_CONFIRM);
        break;
      }
      case NearbySyncExchange::State::WaitingForAck:
        line(tr(STR_NEARBY_SENDING_STATS), true);
        line(labeled(tr(STR_NEARBY_TO_DEVICE),
                     exchange_.peerName().empty() ? tr(STR_NEARBY_UNKNOWN_READER) : exchange_.peerName()));
        line(formatStatsTime(localStats_));
        line(formatStatsSummary(localStats_));
        line(formatStatsHistory(localStats_));
        line(tr(STR_NEARBY_WAITING_FOR_RECEIVER));
        break;
      case NearbySyncExchange::State::WaitingForOffer:
        line(tr(STR_NEARBY_WAITING_FOR_SENDER), true);
        line(labeled(tr(STR_NEARBY_FROM_DEVICE),
                     exchange_.peerName().empty() ? tr(STR_NEARBY_UNKNOWN_READER) : exchange_.peerName()));
        break;
      case NearbySyncExchange::State::OfferReady:
        line(tr(STR_NEARBY_STATS_FOUND), true);
        line(labeled(tr(STR_NEARBY_FROM_DEVICE),
                     exchange_.peerName().empty() ? tr(STR_NEARBY_UNKNOWN_READER) : exchange_.peerName()));
        line(formatStatsTime(peerStats_));
        line(formatStatsSummary(peerStats_));
        line(formatStatsHistory(peerStats_));
        if (peerStatsStorageProtected_) {
          line(tr(STR_NEARBY_STATS_PROTECTED_BLOCKED), true);
        } else if (peerStatsRegresses_) {
          line(tr(STR_NEARBY_STATS_REGRESSION_BLOCKED), true);
        } else {
          line(tr(STR_NEARBY_STATS_SAVE_HINT));
          confirmLabel = tr(STR_NEARBY_SAVE);
        }
        break;
      case NearbySyncExchange::State::WaitingForComplete:
        line(tr(STR_NEARBY_WAITING_FOR_SENDER), true);
        break;
      case NearbySyncExchange::State::Accepted:
        line(exchange_.role() == NearbySync::Role::Sender ? tr(STR_NEARBY_STATS_SENT) : tr(STR_NEARBY_STATS_SAVED),
             true);
        line(tr(STR_NEARBY_RESTARTING));
        break;
      case NearbySyncExchange::State::Error:
        break;
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), confirmLabel.c_str(), previousLabel.c_str(), nextLabel.c_str());
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

bool NearbyStatsSyncActivity::preventAutoSleep() {
  return exchange_.state() != NearbySyncExchange::State::Idle && exchange_.state() != NearbySyncExchange::State::Error;
}

bool NearbyStatsSyncActivity::skipLoopDelay() {
  return exchange_.state() == NearbySyncExchange::State::Discovering ||
         exchange_.state() == NearbySyncExchange::State::WaitingForAck ||
         exchange_.state() == NearbySyncExchange::State::WaitingForOffer ||
         exchange_.state() == NearbySyncExchange::State::WaitingForComplete;
}

void NearbyStatsSyncActivity::startExchange(const NearbySync::Role role) {
  if (role == NearbySync::Role::Sender && !localStatsTrusted_) {
    setError(tr(STR_NEARBY_ERROR_LOCAL_STATS_INVALID));
    return;
  }
  const bool sender = role == NearbySync::Role::Sender;
  if (!exchange_.start(NearbySync::Kind::Stats, role, localMac_, deviceName(localMac_),
                       sender ? localOffer_.data() : nullptr, sender ? localOffer_.size() : 0, millis())) {
    setError(exchangeErrorMessage(exchange_.error()));
  }
  requestUpdate();
}

bool NearbyStatsSyncActivity::preparePeerStats() {
  if (peerStatsReady_) return true;
  if (exchange_.peerOfferSize() != GlobalReadingStats::CURRENT_FILE_SIZE ||
      ReadingStatsCodec::decode(exchange_.peerOffer(), exchange_.peerOfferSize(), peerStats_) !=
          ReadingStatsDecodeResult::Ok) {
    setError(tr(STR_NEARBY_ERROR_STATS_INVALID));
    return false;
  }
  peerStatsReady_ = true;
  inspectPeerStatsStorage();
  return true;
}

void NearbyStatsSyncActivity::inspectPeerStatsStorage() {
  peerStatsRegresses_ = false;
  peerStatsStorageProtected_ = false;

  const std::string path = statsPath(exchange_.peerDeviceMac());
  const std::string legacyPath = legacyStatsPath(exchange_.peerDeviceMac());
  const NearbyStatsStorage::Inspection inspection = NearbyStatsStorage::inspect(path, legacyPath, peerStats_);
  peerStatsStorageProtected_ = inspection.protectedStorage;
  peerStatsRegresses_ = inspection.regresses;
}

bool NearbyStatsSyncActivity::savePeerStats() {
  if (!preparePeerStats() || !Storage.ensureDirectoryExists(CROSSPOINT_ROOT) ||
      !Storage.ensureDirectoryExists(SYNCED_STATS_DIR)) {
    return false;
  }

  const std::string path = statsPath(exchange_.peerDeviceMac());
  const std::string legacyPath = legacyStatsPath(exchange_.peerDeviceMac());
  NearbyStatsStorage::Inspection inspection;
  const bool saved = NearbyStatsStorage::saveRawSnapshot(path, legacyPath, exchange_.peerOffer(),
                                                         exchange_.peerOfferSize(), &inspection);
  peerStatsStorageProtected_ = inspection.protectedStorage;
  peerStatsRegresses_ = inspection.regresses;
  if (!saved) {
    LOG_ERR(LOG_TAG, "Refusing unsafe peer stats replacement: %s", path.c_str());
    return false;
  }
  return true;
}

void NearbyStatsSyncActivity::acceptPeerStats() {
  if (!savePeerStats()) {
    if (peerStatsStorageProtected_) {
      setError(tr(STR_NEARBY_STATS_PROTECTED_BLOCKED));
    } else if (peerStatsRegresses_) {
      setError(tr(STR_NEARBY_STATS_REGRESSION_BLOCKED));
    } else {
      setError(tr(STR_NEARBY_ERROR_STATS_SAVE));
    }
    return;
  }
  if (!exchange_.acknowledgePeerOffer(millis())) setError(tr(STR_NEARBY_ERROR_PROTOCOL));
}

void NearbyStatsSyncActivity::handleExchangeState() {
  if (exchange_.state() == NearbySyncExchange::State::Error && errorMessage_.empty()) {
    setError(exchangeErrorMessage(exchange_.error()));
    return;
  }
  if (exchange_.state() == NearbySyncExchange::State::OfferReady && !peerStatsReady_ && !preparePeerStats()) return;
  if (lastExchangeState_ != exchange_.state()) {
    lastExchangeState_ = exchange_.state();
    requestUpdate();
  }
}

void NearbyStatsSyncActivity::setError(const std::string& message) {
  LOG_ERR(LOG_TAG, "%s", message.c_str());
  errorMessage_ = message.empty() ? tr(STR_NEARBY_ERROR_PROTOCOL) : message;
  exchange_.stop();
  requestUpdate();
}

void NearbyStatsSyncActivity::exitActivity() { finish(); }
