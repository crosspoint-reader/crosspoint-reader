#include "NearbyStatsSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#ifndef SIMULATOR
#include <esp_mac.h>
#endif

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/reader/ReadingStatsCodec.h"
#include "activities/reader/ReadingStatsStorage.h"
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

bool inspectReplaceableStatsFile(const std::string& path, bool* currentFormat = nullptr) {
  if (currentFormat) *currentFormat = false;
  ReadingStatsCodec::GlobalBytes bytes{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path.c_str(), bytes.data(), bytes.size());
  if (ReadingStatsStorage::isProtectedExistingFile(read.result, false)) return false;
  if (read.result != ReadingStatsStorage::ReadResult::Ok) return true;

  GlobalReadingStats decoded;
  const ReadingStatsDecodeResult result = ReadingStatsCodec::decode(bytes.data(), read.size, decoded);
  if (ReadingStatsStorage::isProtectedExistingFile(read.result, result == ReadingStatsDecodeResult::NewerFormat)) {
    return false;
  }
  if (currentFormat) *currentFormat = result == ReadingStatsDecodeResult::Ok;
  return true;
}
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
  if (!GlobalReadingStats::isTrustedLoadStatus(statsStatus)) {
    if (errorMessage_.empty()) setError(tr(STR_NEARBY_ERROR_LOCAL_STATS_INVALID));
  } else {
    localOffer_ = ReadingStatsCodec::encode(localStats_);
  }
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
  if (!mappedInput.wasPressed(MappedInputManager::Button::Confirm) || !errorMessage_.empty()) return;
  switch (exchange_.state()) {
    case NearbySyncExchange::State::Idle:
      startExchange();
      break;
    case NearbySyncExchange::State::Pairing:
      if (!exchange_.confirmShare(now)) setError(tr(STR_NEARBY_ERROR_PROTOCOL));
      break;
    case NearbySyncExchange::State::OfferReady:
      acceptPeerStats();
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
    UITheme::drawCenteredText(renderer, screen, bold ? UI_10_FONT_ID : SMALL_FONT_ID, y, text.c_str(), true,
                              bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(bold ? UI_10_FONT_ID : SMALL_FONT_ID) + metrics.verticalSpacing;
  };

  std::string confirmLabel;
  if (!errorMessage_.empty()) {
    line(tr(STR_ERROR_MSG), true);
    line(errorMessage_);
  } else {
    switch (exchange_.state()) {
      case NearbySyncExchange::State::Idle:
        line(tr(STR_NEARBY_STATS_READY), true);
        line(formatStatsSummary(localStats_));
        line(tr(STR_NEARBY_MANUAL_ONLY));
        line(tr(STR_NEARBY_TRUST_WARNING));
        confirmLabel = tr(STR_NEARBY_START);
        break;
      case NearbySyncExchange::State::Discovering:
        line(tr(STR_NEARBY_DISCOVERING), true);
        line(tr(STR_NEARBY_OPEN_ON_BOTH));
        break;
      case NearbySyncExchange::State::Pairing: {
        line(tr(STR_NEARBY_VERIFY_CODE), true);
        line(exchange_.peerName().empty() ? tr(STR_NEARBY_UNKNOWN_READER) : exchange_.peerName());
        char code[16];
        snprintf(code, sizeof(code), "%04u", static_cast<unsigned>(exchange_.pairingCode()));
        line(code, true);
        line(tr(STR_NEARBY_CODE_HINT));
        confirmLabel = tr(STR_NEARBY_SHARE);
        break;
      }
      case NearbySyncExchange::State::WaitingForOffer:
        line(tr(STR_NEARBY_WAITING), true);
        line(exchange_.peerAcceptedLocalOffer() ? tr(STR_NEARBY_PEER_ACCEPTED) : tr(STR_NEARBY_WAITING_HINT));
        break;
      case NearbySyncExchange::State::OfferReady:
        line(tr(STR_NEARBY_STATS_FOUND), true);
        line(formatStatsSummary(peerStats_));
        line(tr(STR_NEARBY_STATS_SAVE_HINT));
        confirmLabel = tr(STR_NEARBY_SAVE);
        break;
      case NearbySyncExchange::State::Accepted:
        line(tr(STR_NEARBY_STATS_SAVED), true);
        line(tr(STR_NEARBY_RESTARTING));
        break;
      case NearbySyncExchange::State::Error:
        break;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel.c_str(), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

bool NearbyStatsSyncActivity::preventAutoSleep() {
  return exchange_.state() != NearbySyncExchange::State::Idle && exchange_.state() != NearbySyncExchange::State::Error;
}

bool NearbyStatsSyncActivity::skipLoopDelay() {
  return exchange_.state() == NearbySyncExchange::State::Discovering ||
         exchange_.state() == NearbySyncExchange::State::WaitingForOffer;
}

void NearbyStatsSyncActivity::startExchange() {
  if (!exchange_.start(NearbySync::Kind::Stats, localMac_, "CrossVi", localOffer_.data(), localOffer_.size(),
                       millis())) {
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
  return true;
}

bool NearbyStatsSyncActivity::savePeerStats() {
  if (!preparePeerStats() || !Storage.ensureDirectoryExists(CROSSPOINT_ROOT) ||
      !Storage.ensureDirectoryExists(SYNCED_STATS_DIR)) {
    return false;
  }

  const std::string path = statsPath(exchange_.peerDeviceMac());
  const std::string tempPath = path + ".tmp";
  const std::string backupPath = path + ".bak";
  bool rotateExisting = false;
  if (!inspectReplaceableStatsFile(path, &rotateExisting) || !inspectReplaceableStatsFile(tempPath) ||
      !inspectReplaceableStatsFile(backupPath)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite unreadable or newer peer stats: %s", path.c_str());
    return false;
  }
  return ReadingStatsStorage::writeAtomic(path.c_str(), backupPath.c_str(), rotateExisting, exchange_.peerOffer(),
                                          exchange_.peerOfferSize());
}

void NearbyStatsSyncActivity::acceptPeerStats() {
  if (!savePeerStats()) {
    setError(tr(STR_NEARBY_ERROR_STATS_SAVE));
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
