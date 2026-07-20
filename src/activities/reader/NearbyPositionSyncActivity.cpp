#include "NearbyPositionSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#ifndef SIMULATOR
#include <esp_mac.h>
#endif

#include "Epub/Section.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NearbyDocumentFingerprint.h"

namespace {
constexpr char LOG_TAG[] = "CVNPOS";

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

std::string formatPage(const CrossPointPosition& position, const float percentage) {
  char value[56];
  snprintf(value, sizeof(value), tr(STR_NEARBY_POSITION_FORMAT), position.pageNumber + 1, position.totalPages,
           static_cast<double>(percentage * 100.0F));
  return value;
}
}  // namespace

NearbyPositionSyncActivity::NearbyPositionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::shared_ptr<Epub> epub,
                                                       const CrossPointPosition localPosition,
                                                       SavedProgressPosition localSavedPosition,
                                                       std::string localChapterName,
                                                       const NearbyReaderLayout localLayout)
    : Activity("NearbyPositionSync", renderer, mappedInput),
      epub_(std::move(epub)),
      localPosition_(localPosition),
      localSavedPosition_(std::move(localSavedPosition)),
      localChapterName_(std::move(localChapterName)),
      localLayout_(localLayout) {}

void NearbyPositionSyncActivity::onEnter() {
  Activity::onEnter();
#ifdef SIMULATOR
  setError(tr(STR_NEARBY_ERROR_SIMULATOR));
#else
  if (esp_efuse_mac_get_default(localMac_.data()) != ESP_OK) setError(tr(STR_NEARBY_ERROR_DEVICE_ID));
#endif
  requestUpdate();
}

void NearbyPositionSyncActivity::onExit() {
  const bool restartReader = exchange_.wasRadioActivated();
  exchange_.stop();
  Activity::onExit();
  if (restartReader) silentRestartToReader();
}

void NearbyPositionSyncActivity::loop() {
  // render() runs on a separate task and observes exchange state, strings and
  // decoded position data. Keep each input/radio update atomic with rendering.
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
      // startExchange() must show the preparing screen synchronously, so it
      // manages its own lock and cannot be called while this one is held.
      lock.unlock();
      startExchange();
      return;
    case NearbySyncExchange::State::Pairing:
      if (!exchange_.confirmShare(now)) setError(tr(STR_NEARBY_ERROR_PROTOCOL));
      break;
    case NearbySyncExchange::State::OfferReady:
      applyPeerPosition();
      break;
    default:
      break;
  }
  handleExchangeState();
}

void NearbyPositionSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_NEARBY_POSITION_SYNC));

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
  } else if (preparing_) {
    line(tr(STR_NEARBY_PREPARING), true);
    line(tr(STR_NEARBY_FINGERPRINT_HINT));
  } else {
    switch (exchange_.state()) {
      case NearbySyncExchange::State::Idle:
        line(tr(STR_NEARBY_POSITION_READY), true);
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
        line(tr(STR_NEARBY_POSITION_FOUND), true);
        line(std::string(tr(STR_LOCAL_LABEL)) + " " + localChapterName_);
        line(formatPage(localPosition_, localSavedPosition_.percentage));
        line(std::string(tr(STR_REMOTE_LABEL)) + " " + peerChapterName_);
        line(formatPage(peerPosition_,
                        static_cast<float>(peerOffer_.percentageQ) / static_cast<float>(NearbySync::PERCENTAGE_SCALE)));
        line(tr(STR_NEARBY_APPLY_POSITION_HINT));
        confirmLabel = tr(STR_NEARBY_APPLY);
        break;
      case NearbySyncExchange::State::Accepted:
        line(tr(STR_NEARBY_POSITION_SAVED), true);
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

bool NearbyPositionSyncActivity::preventAutoSleep() {
  return exchange_.state() != NearbySyncExchange::State::Idle && exchange_.state() != NearbySyncExchange::State::Error;
}

bool NearbyPositionSyncActivity::skipLoopDelay() {
  return exchange_.state() == NearbySyncExchange::State::Discovering ||
         exchange_.state() == NearbySyncExchange::State::WaitingForOffer;
}

bool NearbyPositionSyncActivity::prepareLocalOffer() {
  if (!epub_) return false;
  if (localPosition_.spineIndex < 0 || localPosition_.spineIndex > UINT16_MAX || localPosition_.pageNumber < 0 ||
      localPosition_.pageNumber >= UINT16_MAX || !localPosition_.hasParagraphIndex ||
      localPosition_.paragraphIndex == 0 || localLayout_.viewportWidth == 0 || localLayout_.viewportHeight == 0 ||
      !std::isfinite(localSavedPosition_.percentage)) {
    setError(tr(STR_NEARBY_ERROR_POSITION_NOT_READY));
    return false;
  }
  documentFingerprint_ = calculateNearbyDocumentFingerprint(epub_->getPath());
  if (documentFingerprint_.empty()) return false;

  NearbySync::PositionOffer offer;
  offer.documentHash = documentFingerprint_;
  offer.percentageQ = static_cast<uint32_t>(std::lround(std::clamp(localSavedPosition_.percentage, 0.0F, 1.0F) *
                                                        static_cast<float>(NearbySync::PERCENTAGE_SCALE)));
  offer.spineIndex = static_cast<uint16_t>(localPosition_.spineIndex);
  offer.pageNumber = static_cast<uint16_t>(localPosition_.pageNumber);
  offer.totalPages = static_cast<uint16_t>(
      std::clamp(std::max(localPosition_.totalPages, localPosition_.pageNumber + 1), 1, static_cast<int>(UINT16_MAX)));
  localPosition_.totalPages = offer.totalPages;
  offer.paragraphIndex = localPosition_.paragraphIndex;
  offer.hasParagraphIndex = true;
  if (localSavedPosition_.xpath.size() <= NearbySync::MAX_XPATH_BYTES) offer.xpath = localSavedPosition_.xpath;
  return NearbySync::encodePositionOffer(offer, localOfferBytes_.data(), localOfferBytes_.size(), localOfferSize_);
}

bool NearbyPositionSyncActivity::preparePeerPosition() {
  if (peerPositionReady_) return true;
  if (!NearbySync::decodePositionOffer(exchange_.peerOffer(), exchange_.peerOfferSize(), peerOffer_) || !epub_) {
    setError(tr(STR_NEARBY_ERROR_PROTOCOL));
    return false;
  }
  if (documentFingerprint_.empty()) {
    setError(tr(STR_NEARBY_ERROR_FINGERPRINT));
    return false;
  }
  if (peerOffer_.documentHash != documentFingerprint_) {
    setError(tr(STR_NEARBY_ERROR_DIFFERENT_BOOK));
    return false;
  }

  if (!peerOffer_.hasParagraphIndex || peerOffer_.paragraphIndex == 0 ||
      peerOffer_.spineIndex >= epub_->getSpineItemsCount()) {
    setError(tr(STR_NEARBY_ERROR_POSITION_NOT_READY));
    return false;
  }

  // Validate the target section cache against the exact per-book layout that
  // was active when Nearby was opened. Page-count equality alone does not prove
  // equal pagination; the peer position is therefore resolved only through the
  // local paragraph LUT, never by copying/scaling a page number.
  Section targetSection(epub_, peerOffer_.spineIndex, renderer);
  const bool cacheMatches = targetSection.loadSectionFile(
      localLayout_.fontId, localLayout_.lineCompression, localLayout_.extraParagraphSpacing,
      localLayout_.paragraphAlignment, localLayout_.viewportWidth, localLayout_.viewportHeight,
      localLayout_.hyphenationEnabled, localLayout_.embeddedStyle, localLayout_.imageRendering,
      localLayout_.focusReadingEnabled);
  if (!cacheMatches || targetSection.isPartial() || targetSection.pageCount == 0) {
    setError(tr(STR_NEARBY_ERROR_POSITION_NOT_READY));
    return false;
  }
  const auto targetPage = targetSection.getUniquePageForParagraphIndex(peerOffer_.paragraphIndex);
  if (!targetPage.has_value() || *targetPage >= targetSection.pageCount) {
    setError(tr(STR_NEARBY_ERROR_POSITION_NOT_READY));
    return false;
  }
  peerPosition_ = {static_cast<int>(peerOffer_.spineIndex), static_cast<int>(*targetPage),
                   static_cast<int>(targetSection.pageCount)};
  peerPosition_.paragraphIndex = peerOffer_.paragraphIndex;
  peerPosition_.hasParagraphIndex = true;
  const int tocIndex = epub_->getTocIndexForSpineIndex(peerPosition_.spineIndex);
  peerChapterName_ = tocIndex >= 0 ? epub_->getTocItem(tocIndex).title : tr(STR_NEARBY_UNKNOWN_CHAPTER);
  peerPositionReady_ = true;
  return true;
}

void NearbyPositionSyncActivity::startExchange() {
  {
    RenderLock lock(*this);
    preparing_ = true;
  }
  requestUpdateAndWait();

  RenderLock lock(*this);
  if (!prepareLocalOffer()) {
    preparing_ = false;
    if (errorMessage_.empty()) setError(tr(STR_NEARBY_ERROR_FINGERPRINT));
    return;
  }
  preparing_ = false;
  if (!exchange_.start(NearbySync::Kind::Position, localMac_, "CrossVi", localOfferBytes_.data(), localOfferSize_,
                       millis())) {
    setError(exchangeErrorMessage(exchange_.error()));
  }
  requestUpdate();
}

void NearbyPositionSyncActivity::applyPeerPosition() {
  if (!preparePeerPosition()) return;
  if (!EpubReaderUtils::saveProgress(*epub_, peerPosition_.spineIndex, peerPosition_.pageNumber,
                                     peerPosition_.totalPages)) {
    setError(tr(STR_SAVE_PROGRESS_FAILED));
    return;
  }
  if (!exchange_.acknowledgePeerOffer(millis())) setError(tr(STR_NEARBY_ERROR_PROTOCOL));
}

void NearbyPositionSyncActivity::handleExchangeState() {
  if (exchange_.state() == NearbySyncExchange::State::Error && errorMessage_.empty()) {
    setError(exchangeErrorMessage(exchange_.error()));
    return;
  }
  if (exchange_.state() == NearbySyncExchange::State::OfferReady && !peerPositionReady_ && !preparePeerPosition()) {
    return;
  }
  if (lastExchangeState_ != exchange_.state()) {
    lastExchangeState_ = exchange_.state();
    requestUpdate();
  }
}

void NearbyPositionSyncActivity::setError(const std::string& message) {
  LOG_ERR(LOG_TAG, "%s", message.c_str());
  errorMessage_ = message.empty() ? tr(STR_NEARBY_ERROR_PROTOCOL) : message;
  exchange_.stop();
  requestUpdate();
}

void NearbyPositionSyncActivity::exitActivity() {
  activityManager.goToReader(epub_ ? epub_->getPath() : std::string{});
}
