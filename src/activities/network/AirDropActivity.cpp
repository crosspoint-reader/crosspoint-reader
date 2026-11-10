#include "AirDropActivity.h"
#if CROSSPOINT_AIRDROP
#include <FontCacheManager.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

void AirDropActivity::onEnter() {
  Activity::onEnter();
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  WiFi.mode(WIFI_OFF);
  powerManager.setPowerSaving(false);
  startedAt = millis();
  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("AIRDROP", "Starting internal=%zu largest=%zu psram=%zu", internal.freeBytes, internal.largestBlockBytes,
          psram.freeBytes);
  LOG_INF("AIRDROP", "Storage ready=%d root=%d", Storage.ready(), Storage.exists("/"));
  if (!Storage.ensureDirectoryExists("/AirDrop")) {
    LOG_ERR("AIRDROP", "Cannot create destination directory before radio startup");
    state = State::Error;
    showState();
    return;
  }
  diagnosticsFailed = !Storage.openFileForWrite("AIRDROP", "/AirDrop/last-transfer.log", diagnostics);
  recordDiagnostics(true);
  if (!ad_prepare_tls()) {
    ad_end();
    state = State::Error;
    showState();
    return;
  }
  LOG_INF("AIRDROP", "TLS prepared internal=%zu largest=%zu", HalMemory::getInternalHeap().freeBytes,
          HalMemory::getInternalHeap().largestBlockBytes);
  radioStarted = awdl_begin() == ESP_OK;
  if (!radioStarted) {
    LOG_ERR("AIRDROP", "Radio startup failed; restarting to release partial initialization");
    silentRestart();
    return;
  }
  if (radioStarted) {
    ad_on_stream(&AirDropActivity::receiveStream, this);
    receiverStarted = ad_begin(RECEIVE_LIMIT) == 0;
  }
  if (!receiverStarted) {
    ad_end();
    state = State::Error;
  } else {
    ad_stats_read(&stats);
    observedErrors = stats.file_err;
    observedLimitRejections = stats.limit_rejections;
  }
  showState();
}
void AirDropActivity::stop() {
  if (receiverStarted && !ad_end()) {
    LOG_ERR("AIRDROP", "Listener did not stop; restarting safely");
    silentRestart();
  }
  receiverStarted = false;
  ad_stats_read(&stats);
  recordDiagnostics(true);
  if (diagnostics) diagnostics.close();
  if (radioStarted && awdl_end() != ESP_OK) {
    LOG_ERR("AIRDROP", "Radio did not stop; restarting safely");
    silentRestart();
  }
  radioStarted = false;
  abortFile();
}
void AirDropActivity::onExit() {
  stop();
  powerManager.setPowerSaving(true);
  Activity::onExit();
  LOG_INF("AIRDROP", "Stopped internal=%zu psram=%zu", HalMemory::getInternalHeap().freeBytes,
          HalMemory::getPsramHeap().freeBytes);
}
bool AirDropActivity::answerRequest(bool accept) {
  if (state != State::Asking || !ad_request_pending()) return false;
  if (!ad_request_answer(accept)) return false;
  if (accept) {
    stats.rx_bytes = 0;
    writtenSize = 0;
    maxWriteMs = 0;
  }
  state = accept ? State::Receiving : State::Waiting;
  lastProgressDraw = millis();
  popupNeedsUpdate = true;
  return true;
}
void AirDropActivity::showState() {
  popupNeedsUpdate = false;
  const char* body = nullptr;
  switch (state) {
    case State::Waiting:
      snprintf(statusLine, sizeof(statusLine), tr(STR_AIRDROP_WAITING), ad_receiver_name());
      body = statusLine;
      break;
    case State::Asking: {
      AdAskInfo request;
      if (ad_request_info(&request)) {
        if (request.fileCount > 1)
          snprintf(statusLine, sizeof(statusLine), tr(STR_AIRDROP_ASK_FILES), request.fileCount, request.filename);
        else
          snprintf(statusLine, sizeof(statusLine), tr(STR_AIRDROP_ASK_FILE), request.filename);
        body = statusLine;
      } else
        body = tr(STR_AIRDROP_ASK);
      break;
    }
    case State::TooLarge:
      body = tr(STR_AIRDROP_TOO_LARGE);
      break;
    case State::Receiving:
      snprintf(statusLine, sizeof(statusLine), "%s\n%lu KiB / %lu KiB%s%s",
               stats.upload_pending || stats.rx_bytes ? tr(STR_AIRDROP_PROGRESS) : tr(STR_AIRDROP_WAIT_UPLOAD),
               static_cast<unsigned long>(stats.rx_bytes / 1024), static_cast<unsigned long>(writtenSize / 1024),
               diagnosticsFailed ? "\n" : "", diagnosticsFailed ? tr(STR_AIRDROP_LOG_UNAVAILABLE) : "");
      body = statusLine;
      break;
    case State::Saved:
      body = savedPath;
      break;
    case State::Error:
      snprintf(statusLine, sizeof(statusLine), "%s\n%s", tr(STR_AIRDROP_FAILED),
               diagnosticsFailed ? tr(STR_AIRDROP_LOG_UNAVAILABLE) : tr(STR_AIRDROP_LOG_HINT));
      body = statusLine;
      break;
  }
  const char* options[] = {tr(STR_AIRDROP_STOP), tr(STR_AIRDROP_OPEN)};
  int count = state == State::Saved ? 2 : 1;
  if (state == State::Asking) {
    options[0] = tr(STR_AIRDROP_DECLINE);
    options[1] = tr(STR_AIRDROP_ACCEPT);
    count = 2;
  }
  if (state == State::TooLarge) options[0] = tr(STR_OK_BUTTON);
  popup.show(tr(STR_AIRDROP), body, options, count, 0, [this](int index) {
    if (state == State::TooLarge) {
      state = State::Waiting;
      popupNeedsUpdate = true;
    } else if (state == State::Asking) {
      answerRequest(index == 1);
    } else {
      leaving = true;
      if (state == State::Saved && index == 1) {
        stop();
        // File browser supports every received type; its book actions open supported formats.
        activityManager.goToFileBrowser("/AirDrop");
      } else
        activityManager.goHome();
    }
  });
  requestUpdate();
}
bool AirDropActivity::receiveStream(void* ctx, AdStreamEvent event, const uint8_t* data, uint32_t len) {
  auto* self = static_cast<AirDropActivity*>(ctx);
  switch (event) {
    case AdStreamEvent::Begin:
      self->abortFile();
      self->archive.reset({self,
                           [](void* p, const char* name, uint32_t size) {
                             return static_cast<AirDropActivity*>(p)->beginFile(name, size);
                           },
                           [](void* p, const uint8_t* bytes, uint32_t size) {
                             return static_cast<AirDropActivity*>(p)->writeFile(bytes, size);
                           },
                           [](void* p) { return static_cast<AirDropActivity*>(p)->endFile(); }},
                          RECEIVE_LIMIT);
      return true;
    case AdStreamEvent::Data: {
      const bool ok = self->archive.feed(data, len);
      if (!ok && self->archive.tooLarge()) ad_reject_too_large();
      return ok;
    }
    case AdStreamEvent::Finish:
      if (!self->archive.complete()) return false;
      self->state = State::Saved;
      return true;
    case AdStreamEvent::Abort:
      self->abortFile();
      if (self->state != State::TooLarge) self->state = State::Error;
      return true;
  }
  return false;
}
void AirDropActivity::abortFile() {
  if (output) output.close();
  if (temporaryOwned) Storage.remove(temporaryPath);
  temporaryOwned = false;
  temporaryPath[0] = 0;
}
bool AirDropActivity::beginFile(const char* name, uint32_t len) {
  LOG_INF("AIRDROP", "Begin file name=%s bytes=%u", name, len);
  if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    if (*p < 32 || *p == '/' || *p == '\\' || *p == ':') return false;
  if (!Storage.exists("/AirDrop") && !Storage.mkdir("/AirDrop")) return false;
  snprintf(savedPath, sizeof(savedPath), "/AirDrop/%s", name);
  unsigned collision = 0;
  while (Storage.exists(savedPath)) {
    if (++collision > 9999) return false;
    snprintf(temporaryPath, sizeof(temporaryPath), "/AirDrop/%u", collision);
    if (Storage.exists(temporaryPath)) continue;
    if (!Storage.mkdir(temporaryPath)) return false;
    snprintf(savedPath, sizeof(savedPath), "/AirDrop/%u/%s", collision, name);
  }
  // A separate short staging name preserves the filesystem's full basename limit.
  unsigned staging = 0;
  do {
    if (++staging > 9999) return false;
    snprintf(temporaryPath, sizeof(temporaryPath), "/AirDrop/.incoming-%u.part", staging);
  } while (Storage.exists(temporaryPath));
  temporaryOwned = true;
  if (!Storage.openFileForWrite("AIRDROP", temporaryPath, output)) return false;
  expectedSize = len;
  writtenSize = 0;
  return true;
}
bool AirDropActivity::writeFile(const uint8_t* data, uint32_t len) {
  if (!output || len > expectedSize - writtenSize) return false;
  for (uint32_t offset = 0; offset < len;) {
    const size_t chunk = std::min<uint32_t>(4096, len - offset);
    const uint32_t writeStarted = millis();
    const size_t saved = output.write(data + offset, chunk);
    maxWriteMs = std::max<uint32_t>(maxWriteMs, millis() - writeStarted);
    if (saved != chunk) return false;
    offset += chunk;
    writtenSize += chunk;
    delay(1);
  }
  return true;
}
bool AirDropActivity::endFile() {
  if (!output) return false;
  output.flush();
  bool ok = writtenSize == expectedSize && output.size() == expectedSize;
  ok = output.close() && ok;
  if (ok) ok = Storage.rename(temporaryPath, savedPath);
  if (!ok) {
    abortFile();
    return false;
  }
  temporaryPath[0] = 0;
  temporaryOwned = false;
  received++;
  LOG_INF("AIRDROP", "Saved %s bytes=%u", savedPath, expectedSize);
  return true;
}

void AirDropActivity::recordDiagnostics(bool force) {
  if (!diagnostics || (!force && millis() - lastDiagnostic < 10000)) return;
  lastDiagnostic = millis();
  AwdlTransportStats radio{};
  if (radioStarted) awdl_transport_stats(&radio);
  char line[256];
  const int n = snprintf(
      line, sizeof(line),
      "%lu state=%u wait=%u upload=%u rx=%lu saved=%lu wire=%lu tcp=%lu tls=%lu fail=%lu/%d "
      "ask=%lu err=%lu lock=%u mask=%04x drop=%lu q=%u flight=%u ackskip=%lu txdrop=%lu heap=%u sdms=%lu reason=%s\n",
      static_cast<unsigned long>(millis()), static_cast<unsigned>(state), stats.awaiting_upload, stats.upload_pending,
      static_cast<unsigned long>(stats.rx_bytes), static_cast<unsigned long>(writtenSize),
      static_cast<unsigned long>(stats.socket_rx_bytes), static_cast<unsigned long>(stats.tcp_accept),
      static_cast<unsigned long>(stats.tls_ok), static_cast<unsigned long>(stats.tls_failures), stats.last_tls_error,
      static_cast<unsigned long>(stats.ask_ok), static_cast<unsigned long>(stats.file_err), radio.locked,
      radio.channelMask, static_cast<unsigned long>(radio.ringDrops), radio.queued, radio.inflight,
      static_cast<unsigned long>(radio.acksSkipped), static_cast<unsigned long>(radio.txQueueDrops), ESP.getFreeHeap(),
      static_cast<unsigned long>(maxWriteMs), stats.file_errmsg);
  if (n >= static_cast<int>(sizeof(line))) line[sizeof(line) - 2] = '\n';
  const size_t bytes = n > 0 ? std::min<size_t>(n, sizeof(line) - 1) : 0;
  if (diagnosticBytes + bytes > 32768) {
    diagnostics.close();
    return;
  }
  if (!bytes || diagnostics.write(line, bytes) != bytes) {
    diagnosticsFailed = true;
    popupNeedsUpdate = true;
    diagnostics.close();
    return;
  }
  diagnosticBytes += bytes;
  diagnostics.flush();
}

void AirDropActivity::loop() {
  if (leaving) return;
  const auto before = state;
  if (receiverStarted) {
    ad_poll();
    if (ad_request_pending())
      state = State::Asking;
    else if (state == State::Asking)
      state = State::Waiting;
    if (millis() - lastStats > 1000) {
      lastStats = millis();
      ad_stats_read(&stats);
      if (stats.file_err != observedErrors) {
        observedErrors = stats.file_err;
        if (state != State::TooLarge) state = State::Error;
      }
      if (stats.limit_rejections != observedLimitRejections) {
        observedLimitRejections = stats.limit_rejections;
        state = State::TooLarge;
      }
      recordDiagnostics(state != before);
      if (state == State::Receiving && millis() - lastProgressDraw >= 10000) {
        lastProgressDraw = millis();
        popupNeedsUpdate = true;
      }
      LOG_DBG("AIRDROP", "discover=%u ask=%u rx=%u saved=%u heap=%u", stats.discover_ok, stats.ask_ok, stats.rx_bytes,
              received, ESP.getFreeHeap());
    }
  }
  if (state != before || popupNeedsUpdate) {
    RenderLock lock;
    showState();
  }
  if (millis() - startedAt >= 600000 && state == State::Waiting && !ad_upload_pending()) {
    leaving = true;
    activityManager.goHome();
    return;
  }
  {
    RenderLock lock;
    const auto beforeInput = state;
    popup.handleInput(mappedInput, [this] { requestUpdate(); });
    if (state != beforeInput && !leaving) showState();
    if (!popup.isActive() && !leaving) {
      if (state == State::Asking) {
        ad_request_answer(false);
        state = State::Waiting;
        showState();
      } else {
        leaving = true;
        activityManager.goHome();
      }
    }
  }
  delay(5);
}
void AirDropActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
#endif
